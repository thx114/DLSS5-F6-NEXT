// SPDX-License-Identifier: GPL-3.0-or-later
#include "GenshinDx12Transport.h"
#include "GenshinCpuStageTiming.h"
#include "GenshinHookCpuTiming.h"
#include "GenshinFsrSubstageProgress.h"
#include <pch.h>
#include <atomic>
#include <dxgi1_6.h>
#include <mutex>
#include <limits>
#include <vector>
#include <Config.h>
#include <genshin/GenshinFgConsumer.h>
#include <integration/HoYoShade.h>
#include <inputs/FG/GenshinBridge_Present_Dx12.h>
#include <native/OpticalFgNrPolicy.h>
#include <hooks/Reflex_Hooks.h>
#include <low_latency/GpuTimeline.h>

namespace GenshinTransport {
using Microsoft::WRL::ComPtr;
namespace {
struct SurfaceShape {UINT width=0,height=0;DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;};
// A group moves with one retired surface slot. Only its checked-out owning
// thread advances the cursors; no other lease can signal these fence objects.
struct SlotFences {
    ComPtr<ID3D11Fence> input11,output11,retire11;
    ComPtr<ID3D12Fence> input12,output12,wait12;
    uint64_t inputCursor=0,waitCursor=0,outputCursor=0,returnedCursor=0;
    // Sticky across every receipt/lifetime alias. Never rehabilitate a failed
    // Signal by observing a later value, even if another caller signals it.
    std::atomic<bool> quarantined{false};
    static uint64_t Next(uint64_t& cursor) noexcept {
        if(cursor>=UINT64_MAX-1)return 0;
        return ++cursor;
    }
    bool Reusable() const noexcept {
        return !quarantined.load(std::memory_order_acquire) &&
            input11 && output11 && retire11 && input12 && output12 && wait12 &&
            inputCursor<UINT64_MAX-1 && waitCursor<UINT64_MAX-1 &&
            outputCursor<UINT64_MAX-1 && returnedCursor<UINT64_MAX-1;
    }
};
// Allocated once before GPU work. Values do not follow mutable slot cursors.
struct FenceValues {
    const uint64_t input,wait,submitted,returned;
    FenceValues(uint64_t i,uint64_t w,uint64_t s,uint64_t r) noexcept:
        input(i),wait(w),submitted(s),returned(r){}
};
template<class Fence> bool Reached(Fence* fence,uint64_t value) noexcept {
    if(!fence || !value || value==UINT64_MAX)return false;
    const auto completed=GenshinHookCpuTiming::Call(GenshinHookCpuTiming::Stage::FenceQueryApi,[&]{return fence->GetCompletedValue();},GenshinHookCpuTiming::ResultKind::Fence);
    return completed!=UINT64_MAX && completed>=value;
}
}
struct Storage {
    GenshinSubstage::Key substageKey=GenshinSubstage::current;
    Observation observation;
    ComPtr<ID3D11Device5> device11;
    ComPtr<ID3D11DeviceContext4> context;
    ComPtr<ID3D12Device> device12; // Creator, never inferred from a child owner.
    GpuWorkDomain workDomain;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandQueue> submittedQueue;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12CommandAllocator> allocator;
    struct ConsumerWork {ComPtr<ID3D12GraphicsCommandList> list;ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12CommandQueue> queue;};
    std::vector<ConsumerWork> consumers;
    ComPtr<ID3D12CommandQueue> rejectedQueue;
    ConsumerWork rejectedConsumer;
    std::array<ComPtr<ID3D11Resource>,4> sources;
    std::array<ComPtr<ID3D11Texture2D>,4> shared;
    std::array<ComPtr<ID3D12Resource>,4> imported;
    std::array<SurfaceShape,4> surfaceShape{};
    std::shared_ptr<const void> surfaceLifetime;
    std::shared_ptr<PreparedSrInputs> preparedInputs;
    std::shared_ptr<PreparedSrOutput> preparedOutput;
    bool directPrepared=false,directOutput=false;
    // Physical completion alone cannot retire immutable packets held by NR/FG.
    // An escaped packet exclusively controls return through surfaceLifetime.
    bool surfaceSetReady=false,escaped=false;
    std::shared_ptr<SlotFences> fences;
    std::shared_ptr<const FenceValues> values;
    bool inputIssued=false,inputFenced=false,executed=false,outputFenced=false;
    bool waitIssued=false,waitFenced=false;
    bool copyIssued=false,copyFenced=false;
    bool inputFlushPending=false; // Pure SR batches input submission with this invocation's Copyback.
    bool outputFlushPending=false,outputFlushIssued=false;
    bool batchAtNativeAcquire=false;
    OutputFilterStage outputFilterStage=OutputFilterStage::None;
    ULONGLONG outputFlushTick=0;
    uint64_t lastConsumer=0;
    // Per-invocation ordering only; a recycled fence group cannot inherit this.
    uint64_t copybackWaitValue=0;
    // Keep registered event handles alive with the GPU quarantine on timeout
    // or failure; never close an event while a pending fence can still use it.
    std::array<HANDLE,2> depthReadyEvents{};
    bool depthWaitAttempted=false;
    ComPtr<ID3D11Query> gpuDisjoint,gpuStart,gpuEnd;
    bool gpuTimingStarted=false,gpuTimingEnded=false;
    uint64_t gpuTimingSampleMs=0;
    // Query resources stay in the invocation's original GPU retention domain,
    // including partial Execute/failed Signal quarantine. No separate owner.
    ComPtr<ID3D12QueryHeap> gpu12Queries;
    ComPtr<ID3D12Resource> gpu12Readback;
    ID3D12GraphicsCommandList* gpu12List=nullptr; // identity; Execute pins the real list
    uint64_t gpu12Frame=0,gpu12MarkerDevice=0,gpu12Hz=0,gpu12CalibrationGpu=0,gpu12CalibrationCpu=0;
    bool gpu12Started=false,gpu12Ended=false,gpu12Collected=false;
    ~Storage();
    Storage* next=nullptr;
    bool Alive() const noexcept {
        return SUCCEEDED(device11->GetDeviceRemovedReason()) && SUCCEEDED(device12->GetDeviceRemovedReason());
    }
    bool Done() const noexcept {
        if(inputFlushPending || outputFlushPending)return false;
        if(fences && fences->quarantined.load(std::memory_order_acquire))return false;
        if(inputIssued && (!inputFenced || !values || !Reached(fences->input11.Get(),values->input)))return false;
        if(waitIssued && (!waitFenced || !values || !Reached(fences->wait12.Get(),values->wait)))return false;
        if(executed && (!outputFenced || !Reached(fences->output12.Get(),lastConsumer)))return false;
        if(copyIssued && (!copyFenced || !values || !Reached(fences->retire11.Get(),values->returned)))return false;
        return true;
    }
};
namespace {
void StartGpuTiming(Storage& s) noexcept {
    static std::atomic<unsigned> sequence{0};
    static std::atomic<uint64_t> nextMonitorSampleMs{0};
    const auto sampleMs=GetTickCount64();
    const auto* config=Config::Instance();
    const bool monitor=config->ShowFps.value_or_default() &&
        config->FpsOverlayType.value_or_default()>=FpsOverlay_ReflexTimings;
    if(monitor) {
        auto next=nextMonitorSampleMs.load(std::memory_order_relaxed);
        if(sampleMs<next || !nextMonitorSampleMs.compare_exchange_strong(next,sampleMs+100,
            std::memory_order_relaxed))return;
    } else if(sequence.fetch_add(1,std::memory_order_relaxed)%64!=2)return;
    // At most ten monitor samples/sec, independent of FPS. Queries never flush/wait.
    s.gpuTimingSampleMs=sampleMs;
    const D3D11_QUERY_DESC timestamp{D3D11_QUERY_TIMESTAMP,0},disjoint{D3D11_QUERY_TIMESTAMP_DISJOINT,0};
    if(FAILED(s.device11->CreateQuery(&timestamp,&s.gpuStart)) || FAILED(s.device11->CreateQuery(&timestamp,&s.gpuEnd)) ||
       FAILED(s.device11->CreateQuery(&disjoint,&s.gpuDisjoint))){s.gpuStart.Reset();s.gpuEnd.Reset();s.gpuDisjoint.Reset();return;}
    s.context->Begin(s.gpuDisjoint.Get());s.context->End(s.gpuStart.Get());s.gpuTimingStarted=true;
}
void FinishGpuTiming(Storage& s) noexcept {
    if(!s.gpuTimingStarted || s.gpuTimingEnded)return;
    s.context->End(s.gpuEnd.Get());s.context->End(s.gpuDisjoint.Get());s.gpuTimingEnded=true;
}
void CollectGpuTimeline(Storage& s) noexcept {
    if(!s.gpu12Ended || s.gpu12Collected || !s.executed || !s.outputFenced ||
       !s.values || !s.fences || s.fences->quarantined.load(std::memory_order_acquire) ||
       !Reached(s.fences->output12.Get(),s.values->submitted))return;
    // This invocation's SR fence completes its Resolve even while later readers
    // are active. Reading it grants no retirement of those reader resources.
    s.gpu12Collected=true;
    if(!s.gpu12Hz || !s.gpu12CalibrationCpu || !s.gpu12Readback)return;
    uint64_t* ticks=nullptr;
    const D3D12_RANGE range{0,2*sizeof(uint64_t)};
    if(FAILED(s.gpu12Readback->Map(0,&range,reinterpret_cast<void**>(&ticks))))return;
    const auto begin=ticks[0],end=ticks[1];
    const D3D12_RANGE written{0,0};s.gpu12Readback->Unmap(0,&written);
    if(end<begin)return;
    GpuTimeline::Publish(GpuTimeline::Stream::Bridge,s.gpu12Frame,
        GpuTimeline::GpuTicksToQpcUs(begin,s.gpu12CalibrationGpu,s.gpu12CalibrationCpu,s.gpu12Hz),
        GpuTimeline::GpuTicksToQpcUs(end,s.gpu12CalibrationGpu,s.gpu12CalibrationCpu,s.gpu12Hz),
        s.gpu12MarkerDevice);
}
void LogGpuTiming(Storage& s) noexcept {
    CollectGpuTimeline(s);
    if(!s.gpuTimingEnded)return;s.gpuTimingEnded=false;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};UINT64 begin=0,end=0;
    const auto flags=D3D11_ASYNC_GETDATA_DONOTFLUSH;
    if(GenshinHookCpuTiming::Call(GenshinHookCpuTiming::Stage::QueryGetDataDontFlush,[&]{return s.context->GetData(s.gpuDisjoint.Get(),&disjoint,sizeof(disjoint),flags);},GenshinHookCpuTiming::ResultKind::Hresult)!=S_OK ||
       GenshinHookCpuTiming::Call(GenshinHookCpuTiming::Stage::QueryGetDataDontFlush,[&]{return s.context->GetData(s.gpuStart.Get(),&begin,sizeof(begin),flags);},GenshinHookCpuTiming::ResultKind::Hresult)!=S_OK ||
       GenshinHookCpuTiming::Call(GenshinHookCpuTiming::Stage::QueryGetDataDontFlush,[&]{return s.context->GetData(s.gpuEnd.Get(),&end,sizeof(end),flags);},GenshinHookCpuTiming::ResultKind::Hresult)!=S_OK){s.gpuTimingEnded=true;return;}
    if(disjoint.Disjoint || !disjoint.Frequency || end<begin){s.gpuStart.Reset();s.gpuEnd.Reset();s.gpuDisjoint.Reset();return;}
    GenshinHookCpuTiming::PublishBridgeGpu(static_cast<double>(end-begin)*1000.0/disjoint.Frequency,
        s.gpuTimingSampleMs);
    static std::atomic<uint64_t> nextReportMs{0};
    const auto now=GetTickCount64();auto next=nextReportMs.load(std::memory_order_relaxed);
    if(now>=next && nextReportMs.compare_exchange_strong(next,now+500,std::memory_order_relaxed))
        LOG_INFO("R981 SR D3D11 GPU: pid={} instance={} generation={} total_ms={:.4f} frequency={} scope=input_transport_model_copyback sample_started_ms={} no_cpu_wait=true",
            GetCurrentProcessId(),s.observation.owner.instance,s.observation.owner.generation,
            static_cast<double>(end-begin)*1000.0/disjoint.Frequency,disjoint.Frequency,s.gpuTimingSampleMs);
    s.gpuStart.Reset();s.gpuEnd.Reset();s.gpuDisjoint.Reset();
}
struct Retention {std::mutex mutex;Storage* head=nullptr;std::atomic<size_t> count{0};};
// Deliberately no process-exit destructor: a failed fence must not release GPU references.
Retention& Retained(){static auto* value=new Retention;return *value;}
thread_local Scope* active=nullptr;
enum class TracePoint : unsigned { Inputs, Witness, OutputSignal, CopybackWait, CopybackSignal, Pending, Complete, Count };

void TraceWitness(Storage& s,const char* phase,const GpuWitness& witness,ID3D12Device* expected,
    const GpuWitness* retirement=nullptr) noexcept {
    static std::atomic<unsigned> reports{0};
    if(reports.fetch_add(1,std::memory_order_relaxed)>=6)return;
    try {LOG_WARN("Genshin SR witness refused: pid={} phase={} reason={} hr={:X} instance={} generation={} "
        "expected_device={} observed_device={} public_owner={} queue={} list={} allocator={} executed={} retirement_fenced={} retire_reason={} retire_hr={:X}",
        GetCurrentProcessId(),phase,witness.reason,static_cast<UINT>(witness.hr),s.observation.owner.instance,
        s.observation.owner.generation,reinterpret_cast<uint64_t>(expected),reinterpret_cast<uint64_t>(witness.actual.Get()),
        reinterpret_cast<uint64_t>(s.workDomain.PublicOwner()),reinterpret_cast<uint64_t>(s.queue.Get()),reinterpret_cast<uint64_t>(s.list.Get()),
        reinterpret_cast<uint64_t>(s.allocator.Get()),s.executed,s.outputFenced,retirement?retirement->reason:"not-issued",
        static_cast<UINT>(retirement?retirement->hr:S_FALSE));}catch(...){}
}

void Trace(Storage& s,TracePoint point,HRESULT hr) noexcept {
    static std::array<std::atomic<unsigned>,static_cast<unsigned>(TracePoint::Count)> counts{};
    if(counts[static_cast<unsigned>(point)].fetch_add(1,std::memory_order_relaxed)>=3)return;
    try {
        const auto& o=s.observation;
        LOG_INFO("Genshin GPU lease: pid={} point={} hr={:X} instance={} generation={} stage={} owner_thread={} "
            "thread={} queue={} submitted_queue={} list={} allocator={} input_issued={} input_fenced={} "
            "wait_issued={} wait_fenced={} executed={} output_fenced={} copy_issued={} copy_fenced={} "
            "input_completed={} wait_completed={} output_completed={} expected_consumer={} retire_completed={} "
            "device11_hr={:X} device12_hr={:X}",GetCurrentProcessId(),static_cast<unsigned>(point),static_cast<UINT>(hr),
            o.owner.instance,o.owner.generation,static_cast<unsigned>(o.stage),o.owner.thread,GetCurrentThreadId(),
            o.queue,reinterpret_cast<uint64_t>(s.submittedQueue.Get()),reinterpret_cast<uint64_t>(s.list.Get()),
            reinterpret_cast<uint64_t>(s.allocator.Get()),s.inputIssued,s.inputFenced,s.waitIssued,s.waitFenced,
            s.executed,s.outputFenced,s.copyIssued,s.copyFenced,GenshinHookCpuTiming::Call(GenshinHookCpuTiming::Stage::FenceQueryApi,[&]{return s.fences->input11->GetCompletedValue();},GenshinHookCpuTiming::ResultKind::Fence),
            GenshinHookCpuTiming::Call(GenshinHookCpuTiming::Stage::FenceQueryApi,[&]{return s.fences->wait12->GetCompletedValue();},GenshinHookCpuTiming::ResultKind::Fence),GenshinHookCpuTiming::Call(GenshinHookCpuTiming::Stage::FenceQueryApi,[&]{return s.fences->output12->GetCompletedValue();},GenshinHookCpuTiming::ResultKind::Fence),s.lastConsumer,GenshinHookCpuTiming::Call(GenshinHookCpuTiming::Stage::FenceQueryApi,[&]{return s.fences->retire11->GetCompletedValue();},GenshinHookCpuTiming::ResultKind::Fence),
            static_cast<UINT>(s.device11->GetDeviceRemovedReason()),static_cast<UINT>(s.device12->GetDeviceRemovedReason()));
    }catch(...){} // Observation must not change ownership or GPU progress.
}
bool Valid(const Owner& o) noexcept {
    return o.device && o.context && o.deviceEpoch && o.producerEpoch && o.configEpoch && o.instance &&
        o.generation && o.historyEpoch && o.thread==GetCurrentThreadId();
}
template<class A,class B> bool SameCom(A* a,B* b) noexcept {
    ComPtr<IUnknown> x,y;
    return a && b && SUCCEEDED(a->QueryInterface(IID_PPV_ARGS(&x))) &&
        SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(&y))) && x.Get()==y.Get();
}
struct PendingOutput {
    std::shared_ptr<Lease> lease;
    Owner owner{};
    ID3D11Device* device=nullptr; // lease owns the device
};
// Bounded owning-thread handoff; no global lock or per-draw registration.
thread_local std::array<PendingOutput,Lease::SurfaceCacheCapacity> pendingOutputs;
bool Compatible(const D3D11_TEXTURE2D_DESC& d) noexcept {
    if(!d.Width || !d.Height || d.MipLevels!=1 || d.ArraySize!=1 || d.SampleDesc.Count!=1 ||
       d.SampleDesc.Quality || (d.BindFlags&D3D11_BIND_DEPTH_STENCIL))return false;
    // Exact-format transport only. No format reinterpretation or depth-convention claim.
    switch(d.Format){
    case DXGI_FORMAT_R32_FLOAT:case DXGI_FORMAT_R16_FLOAT:case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R32G32_FLOAT:case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UINT:case DXGI_FORMAT_R32_UINT:return true;
    default:return false;
    }
}
// The cache owns only retired private surfaces, exact context/domain identities
// and their slot-owned fence group. Source textures, actual GPU work and readers
// remain in Storage/packet owners until retirement. Direct capture identity is
// weak: caching colour/output must not keep a capture slot busy.
struct ReusableSurfaces {
    ComPtr<ID3D11Device5> device11;
    ComPtr<ID3D11DeviceContext4> context;
    ComPtr<ID3D12Device> device12,publicOwner;
    ComPtr<ID3D12CommandQueue> queue;
    std::array<SurfaceShape,4> shape{};
    bool directPrepared=false,directOutput=false;
    std::weak_ptr<PreparedSrInputs> preparedIdentity;
    std::weak_ptr<PreparedSrOutput> outputIdentity;
    std::array<ComPtr<ID3D11Texture2D>,4> shared;
    std::array<ComPtr<ID3D12Resource>,4> imported;
    std::shared_ptr<SlotFences> fences;
};
// Counts actual exported lifetime roots until last packet AND real GPU tail
// retirement. This is ownership publication, never a frame/config revision.
std::atomic<size_t> observationExports{0};
struct ExportedSurfaces : ReusableSurfaces {
    // Unlike the weak cache identity, this is physical reader ownership.
    // Packet destruction may precede GPU completion, so retain it through Done.
    std::shared_ptr<PreparedSrInputs> preparedInputs;
    bool observationHeld=false;
    void ReleaseObservation() noexcept {
        if(observationHeld){observationHeld=false;observationExports.fetch_sub(1,std::memory_order_acq_rel);}
    }
    ~ExportedSurfaces(){ReleaseObservation();}
    std::shared_ptr<const FenceValues> values;
    bool ready=false;
    bool inputIssued=false,inputFenced=false,waitIssued=false,waitFenced=false;
    bool executed=false,copyIssued=false,copyFenced=false;
    std::atomic<bool> outputFenced{false};
    std::atomic<uint64_t> lastConsumer{0};
    ExportedSurfaces* next=nullptr;
    bool Alive() const noexcept {
        return device11 && device12 && SUCCEEDED(device11->GetDeviceRemovedReason()) &&
            SUCCEEDED(device12->GetDeviceRemovedReason());
    }
    bool Done() const noexcept {
        if(!ready || !fences || !values || fences->quarantined.load(std::memory_order_acquire))return false;
        if(inputIssued && (!inputFenced || !Reached(fences->input11.Get(),values->input)))return false;
        if(waitIssued && (!waitFenced || !Reached(fences->wait12.Get(),values->wait)))return false;
        if(executed && (!outputFenced.load(std::memory_order_acquire) ||
                        !Reached(fences->output12.Get(),lastConsumer.load(std::memory_order_acquire))))return false;
        if(copyIssued && (!copyFenced || !Reached(fences->retire11.Get(),values->returned)))return false;
        return true;
    }
};
struct SurfacePool {
    std::mutex mutex;
    // Same width as the existing recording/capture flight ring. Busy/exported
    // leases keep the original independent allocation path, never an idle list
    // of unlimited size or a new global three-lease admission gate.
    std::array<std::unique_ptr<ExportedSurfaces>,Lease::SurfaceCacheCapacity> slots{};
    ExportedSurfaces* pending=nullptr;
};
SurfacePool& Surfaces(){static auto* pool=new SurfacePool;return *pool;}
bool CacheRetiredLocked(SurfacePool& pool,ExportedSurfaces* surfaces) noexcept {
    if(!surfaces->Alive() || !surfaces->fences->Reusable())return false;
    // Callers have observed Done and released all packet owners (or are
    // recycling unexported, completed Storage). Cache only private surfaces;
    // retaining a prepare-slot pin here would exhaust all three capture slots.
    surfaces->preparedInputs.reset();
    if(surfaces->directPrepared)for(size_t i=1;i<=2;++i) {
        surfaces->shared[i].Reset();surfaces->imported[i].Reset();
    }
    for(auto& slot:pool.slots)if(!slot){slot.reset(surfaces);return true;}
    return false;
}
void ReturnExported(ExportedSurfaces* surfaces) noexcept {
    if(!surfaces)return;
    // shared_ptr control-block allocation can fail before the receipt is filled.
    // Such an unpublished object has no GPU work and must never enter the pool.
    if(!surfaces->ready){delete surfaces;return;}
    auto& pool=Surfaces();GenshinHookCpuTiming::Scope r448Wait(GenshinHookCpuTiming::Stage::TransportPoolLock);
    std::lock_guard lock(pool.mutex);r448Wait.Finish();
    GenshinHookCpuTiming::Scope r448Held(GenshinHookCpuTiming::Stage::TransportPoolLocked);
    // Packet count reaching zero is necessary but not GPU completion. Failed
    // Storage retains the actual source/context/queue/work-domain/list/allocator
    // owners and this lifetime, so failure cannot leave only a naked pool token.
    if(surfaces->Done()) {
        surfaces->ReleaseObservation();
        if(!CacheRetiredLocked(pool,surfaces))delete surfaces;
    } else {surfaces->next=pool.pending;pool.pending=surfaces;}
}
void CollectExportedLocked(SurfacePool& pool) noexcept {
    auto** next=&pool.pending;
    while(*next){auto* current=*next;
        if(!current->Done()){next=&current->next;continue;}
        *next=current->next;current->next=nullptr;
        current->ReleaseObservation();
        if(!CacheRetiredLocked(pool,current))delete current;
    }
}
void SaveSurfaceIdentity(ReusableSurfaces& saved,const Storage& s) noexcept {
    saved.device11=s.device11;saved.context=s.context;saved.device12=s.device12;
    saved.publicOwner=s.workDomain.PublicOwner();saved.queue=s.queue;saved.shape=s.surfaceShape;
    saved.directPrepared=s.directPrepared;saved.directOutput=s.directOutput;
    if(s.directPrepared)saved.preparedIdentity=s.preparedInputs;
    if(s.directOutput)saved.outputIdentity=s.preparedOutput;
    saved.fences=s.fences;
}
std::shared_ptr<const void> ExportLifetime(Storage& s) {
    if(s.surfaceLifetime)return s.surfaceLifetime;
    // Direct output retains its existing pure-SR restriction. Direct inputs
    // are immutable to capture while this lifetime owns their actual slot.
    if(!s.surfaceSetReady || s.directOutput || (s.directPrepared && !s.preparedInputs))return {};
    // Create ownership before publishing resources. A failed control-block
    // allocation cannot enqueue an alias and later publish the same slot twice.
    auto lifetime=std::shared_ptr<const void>(new ExportedSurfaces,
        [](const void* value){ReturnExported((ExportedSurfaces*)value);});
    auto* life=const_cast<ExportedSurfaces*>(static_cast<const ExportedSurfaces*>(lifetime.get()));
    SaveSurfaceIdentity(*life,s);life->shared=s.shared;life->imported=s.imported;life->values=s.values;
    if(s.directPrepared)life->preparedInputs=s.preparedInputs;
    life->inputIssued=s.inputIssued;life->inputFenced=s.inputFenced;life->waitIssued=s.waitIssued;life->waitFenced=s.waitFenced;
    life->executed=s.executed;life->outputFenced=s.outputFenced;life->copyIssued=s.copyIssued;life->copyFenced=s.copyFenced;
    life->lastConsumer=s.lastConsumer;life->ready=true;
    observationExports.fetch_add(1,std::memory_order_acq_rel);life->observationHeld=true;
    s.surfaceLifetime=std::move(lifetime);
    return s.surfaceLifetime;
}
void Quarantine(Storage& s) noexcept {
    // Storage::Done stays false forever; Retain keeps real owners, including the
    // direct input/output slots, instead of retaining only a progress number.
    if(s.fences)s.fences->quarantined.store(true,std::memory_order_release);
    if(s.surfaceLifetime) {
        auto* life=const_cast<ExportedSurfaces*>(static_cast<const ExportedSurfaces*>(s.surfaceLifetime.get()));
        life->outputFenced.store(false,std::memory_order_release);
    }
}
enum class SurfacePoint : unsigned {
    Empty,Reused,Published,Escaped,Incomplete,NotAlive,Capacity,WrongDevice,WrongShape,Pending,
    QueuedEscape,CompletedEscape,Unavailable,Count
};
void TraceSurfaces(const Storage& s,SurfacePoint point) noexcept {
    static std::array<std::atomic<unsigned>,static_cast<unsigned>(SurfacePoint::Count)> counts{};
    auto& count=counts[static_cast<unsigned>(point)];auto seen=count.load(std::memory_order_relaxed);
    while(seen<3 && !count.compare_exchange_weak(seen,seen+1,std::memory_order_relaxed)){}
    if(seen>=3)return; // Saturating, at most three messages per reason per process.
    static constexpr const char* names[]={"empty","reused","published","escaped","incomplete","not-alive",
        "capacity","wrong-device-or-queue","wrong-shape","pending-retained","queued-escape","completed-escape","unavailable"};
    try {LOG_INFO("R7 Genshin SR surfaces: pid={} point={} instance={} generation={} ready={} escaped={} capacity=3",
        GetCurrentProcessId(),names[static_cast<unsigned>(point)],s.observation.owner.instance,
        s.observation.owner.generation,s.surfaceSetReady,s.escaped);}catch(...){}
}
bool CanRecycleSurfaces(const Storage& s) noexcept {
    // Once an exported lifetime exists, only that lifetime may publish this
    // set, including failed packet construction where escaped was never set.
    return s.surfaceSetReady && !s.surfaceLifetime && !s.escaped && s.Done() && s.Alive();
}
bool SameSurfaceDevices(const ReusableSurfaces& saved,const Storage& s) noexcept {
    // Exact public interface identities, not canonical-IUnknown or same-adapter
    // substitution. Each Begin still performs the original work-domain witness.
    return saved.device11.Get()==s.device11.Get() && saved.context.Get()==s.context.Get() &&
        saved.device12.Get()==s.device12.Get() && saved.publicOwner.Get()==s.workDomain.PublicOwner() &&
        saved.queue.Get()==s.queue.Get();
}
bool SameSurfaceShape(const std::array<SurfaceShape,4>& a,const std::array<SurfaceShape,4>& b) noexcept {
    for(size_t i=0;i<a.size();++i)
        if(a[i].width!=b[i].width || a[i].height!=b[i].height || a[i].format!=b[i].format)return false;
    return true;
}
bool SamePreparedIdentity(const ReusableSurfaces& saved,const Storage& s) noexcept {
    if(saved.directPrepared!=s.directPrepared || saved.directOutput!=s.directOutput)return false;
    if(s.directPrepared) {
        const auto prepared=saved.preparedIdentity.lock();
        if(!prepared || prepared.get()!=s.preparedInputs.get())return false;
    }
    if(s.directOutput) {
        const auto output=saved.outputIdentity.lock();
        if(!output || output.get()!=s.preparedOutput.get())return false;
    }
    return true;
}
bool TakeSurfaces(Storage& s) noexcept {
    // Search every retired slot before evicting one mismatch. None of the active
    // packet/reader/quarantine lifetimes is visible here or eligible for eviction.
    std::unique_ptr<ExportedSurfaces> saved;SurfacePoint point=SurfacePoint::Empty;
    try {
        auto& pool=Surfaces();
        {GenshinHookCpuTiming::Scope r448Wait(GenshinHookCpuTiming::Stage::TransportPoolLock);
    std::lock_guard lock(pool.mutex);r448Wait.Finish();
    GenshinHookCpuTiming::Scope r448Held(GenshinHookCpuTiming::Stage::TransportPoolLocked);
            CollectExportedLocked(pool);
            size_t mismatch=pool.slots.size();
            for(size_t i=0;i<pool.slots.size();++i) {
                auto& slot=pool.slots[i];if(!slot)continue;
                if(!SameSurfaceDevices(*slot,s)) {
                    if(mismatch==pool.slots.size()){mismatch=i;point=SurfacePoint::WrongDevice;}
                } else if(!SamePreparedIdentity(*slot,s) || !SameSurfaceShape(slot->shape,s.surfaceShape)) {
                    if(mismatch==pool.slots.size()){mismatch=i;point=SurfacePoint::WrongShape;}
                } else if(slot->Done() && slot->Alive() && slot->fences->Reusable()) {
                    saved=std::move(slot);point=SurfacePoint::Reused;break;
                } else if(mismatch==pool.slots.size()){mismatch=i;point=SurfacePoint::NotAlive;}
            }
            if(!saved && mismatch<pool.slots.size())saved=std::move(pool.slots[mismatch]);
        }
        if(point==SurfacePoint::Reused) {
            s.shared=std::move(saved->shared);s.imported=std::move(saved->imported);s.fences=std::move(saved->fences);
        }
        TraceSurfaces(s,point);return point==SurfacePoint::Reused;
    }catch(...){TraceSurfaces(s,SurfacePoint::Unavailable);return false;}
}
void RecycleSurfaces(Storage& s) noexcept {
    if(!CanRecycleSurfaces(s)){
        TraceSurfaces(s,!s.surfaceSetReady?SurfacePoint::Incomplete:s.escaped?SurfacePoint::Escaped:
            !s.Done()?SurfacePoint::Pending:SurfacePoint::NotAlive);return;
    }
    try {
        auto saved=std::make_unique<ExportedSurfaces>();
        SaveSurfaceIdentity(*saved,s);saved->values=s.values;
        if(!saved->fences || !saved->fences->Reusable())return;
        // Capture-owned direct surfaces never enter the retired transport pool.
        // Weak identities above distinguish them without pinning a History slot.
        for(size_t i=0;i<s.shared.size();++i) {
            if((s.directPrepared && (i==1 || i==2)) || (s.directOutput && i==3))continue;
            saved->shared[i]=std::move(s.shared[i]);saved->imported[i]=std::move(s.imported[i]);
        }
        // These are the last successful immutable values. No GPU work is still
        // owned by the cache entry; next Begin reserves fresh values.
        saved->ready=true;
        auto& pool=Surfaces();GenshinHookCpuTiming::Scope r448Wait(GenshinHookCpuTiming::Stage::TransportPoolLock);
    std::lock_guard lock(pool.mutex);r448Wait.Finish();
    GenshinHookCpuTiming::Scope r448Held(GenshinHookCpuTiming::Stage::TransportPoolLocked);
        CollectExportedLocked(pool);
        if(CacheRetiredLocked(pool,saved.get())){saved.release();TraceSurfaces(s,SurfacePoint::Published);}
        else TraceSurfaces(s,SurfacePoint::Capacity);
    }catch(...){TraceSurfaces(s,SurfacePoint::Unavailable);}
}
void Retain(std::unique_ptr<Storage> s) noexcept {
    if(!s || s->Done())return;
    TraceSurfaces(*s,SurfacePoint::Pending);
    auto& r=Retained();GenshinHookCpuTiming::Scope r448Wait(GenshinHookCpuTiming::Stage::TransportRetiredLock);
    std::lock_guard lock(r.mutex);r448Wait.Finish();
    GenshinHookCpuTiming::Scope r448Held(GenshinHookCpuTiming::Stage::TransportRetiredLocked);
    s->next=r.head;r.head=s.release();++r.count;
}
}
Storage::~Storage(){
    // Retain/CollectRetired keep pending invocations intact until final Done destruction.
    RecycleSurfaces(*this);
    for(auto event:depthReadyEvents)if(event)CloseHandle(event);
}
size_t CollectRetired() noexcept {
    auto& r=Retained();GenshinHookCpuTiming::Scope r448Wait(GenshinHookCpuTiming::Stage::TransportRetiredLock);
    std::lock_guard lock(r.mutex);r448Wait.Finish();
    GenshinHookCpuTiming::Scope r448Held(GenshinHookCpuTiming::Stage::TransportRetiredLocked);
    auto** current=&r.head;
    while(*current){auto* value=*current;if(value->Done()){*current=value->next;delete value;--r.count;}
        else current=&value->next;}
    return r.count;
}
size_t RetainedCount() noexcept {auto& r=Retained();GenshinHookCpuTiming::Scope r448Wait(GenshinHookCpuTiming::Stage::TransportRetiredLock);
    std::lock_guard lock(r.mutex);r448Wait.Finish();
    GenshinHookCpuTiming::Scope r448Held(GenshinHookCpuTiming::Stage::TransportRetiredLocked);return r.count;}
bool ObservationRetirementsEmpty() noexcept {
    return Retained().count.load(std::memory_order_acquire)==0 &&
        observationExports.load(std::memory_order_acquire)==0;
}
std::shared_ptr<PreparedSrInputs> PreparedSrInputs::Create(ID3D11DeviceContext* context,UINT width,UINT height) noexcept {
    if(!context || !width || !height || context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return {};
    try {
        auto result=std::shared_ptr<PreparedSrInputs>(new PreparedSrInputs);
        result->context_=context;context->GetDevice(&result->device_);result->thread_=GetCurrentThreadId();
        if(!result->device_ || FAILED(result->device_->GetDeviceRemovedReason()))return {};
        for(size_t i=0;i<2;++i) {
            D3D11_TEXTURE2D_DESC d{};d.Width=width;d.Height=height;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
            d.Format=i==0?DXGI_FORMAT_R32_FLOAT:DXGI_FORMAT_R16G16_FLOAT;d.Usage=D3D11_USAGE_DEFAULT;
            d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
            d.MiscFlags=D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
            if(FAILED(result->device_->CreateTexture2D(&d,nullptr,&result->textures_[i])) ||
               FAILED(result->device_->CreateUnorderedAccessView(result->textures_[i].Get(),nullptr,&result->uavs_[i])))return {};
        }
        return result;
    }catch(...) {return {};}
}
bool PreparedSrInputs::Import(ID3D12Device* device) noexcept {
    if(!device || thread_!=GetCurrentThreadId() || importUnavailable_)return false;
    if(importedDevice_)return importedDevice_.Get()==device && imported_[0] && imported_[1];
    // Publish only the complete pair. Unsupported import uses the existing
    // private transport copies and is not retried every frame.
    std::array<ComPtr<ID3D12Resource>,2> imported;
    for(size_t i=0;i<2;++i) {
        ComPtr<IDXGIResource1> resource;HANDLE handle=nullptr;
        if(FAILED(textures_[i].As(&resource)) || FAILED(resource->CreateSharedHandle(nullptr,
            DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&handle))) {
            importUnavailable_=true;return false;
        }
        const auto hr=device->OpenSharedHandle(handle,IID_PPV_ARGS(&imported[i]));CloseHandle(handle);
        if(FAILED(hr)){importUnavailable_=true;return false;}
    }
    imported_=std::move(imported);importedDevice_=device;return true;
}
bool PreparedSrOutput::PureSr() noexcept {
    const auto& cfg=*Config::Instance();
    return !GenshinFgConsumer::HandoffRequired() && !cfg.DlssNrEnabled.value_or_default() &&
        !native::OpticalFgNrEnabled() && !GenshinBridgeFg::Requested() &&
        !HoYoShade::Selected(HoYoShade::Stage::BeforeSr) &&
        !HoYoShade::Selected(HoYoShade::Stage::BeforeNr) &&
        !HoYoShade::Selected(HoYoShade::Stage::FinalOutput);
}
bool PreparedSrOutput::Authenticate(const Owner& o,ID3D11DeviceContext* context,ID3D11Resource* resource,
    UINT width,UINT height,DXGI_FORMAT format) const noexcept {
    if(!Valid(o) || !PureSr() || !resource || resource!=texture_.Get() || !uav_ || !imported_ ||
       context!=context_.Get() || o.context!=owner_.context || o.device!=owner_.device ||
       o.deviceEpoch!=owner_.deviceEpoch || o.producerEpoch!=owner_.producerEpoch ||
       o.configEpoch!=owner_.configEpoch || o.instance!=owner_.instance || o.historyEpoch!=owner_.historyEpoch ||
       o.thread!=owner_.thread || o.generation<owner_.generation ||
       !context || context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE ||
       width!=desc_.Width || height!=desc_.Height || format!=desc_.Format ||
       !device_ || FAILED(device_->GetDeviceRemovedReason()) || !domain_.Creator() ||
       FAILED(domain_.Creator()->GetDeviceRemovedReason()))return false;
    ComPtr<ID3D11Device> actualDevice;context->GetDevice(&actualDevice);
    ComPtr<ID3D11Resource> actualUav;uav_->GetResource(&actualUav);
    if(actualDevice.Get()!=device_.Get() || !SameCom(actualUav.Get(),texture_.Get()))return false;
    D3D11_TEXTURE2D_DESC d{};texture_->GetDesc(&d);
    if(d.Width!=desc_.Width || d.Height!=desc_.Height || d.Format!=desc_.Format ||
       d.MipLevels!=1 || d.ArraySize!=1 || d.SampleDesc.Count!=1 || d.SampleDesc.Quality ||
       d.Usage!=D3D11_USAGE_DEFAULT || d.CPUAccessFlags ||
       d.BindFlags!=(D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS) ||
       d.MiscFlags!=(D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE))return false;
    const auto imported=imported_->GetDesc();GpuWitness witness;
    return domain_.BoundQueue(domain_.Queue(),witness) &&
        witness.Owns(domain_.Creator(),imported_.Get(),"prepared-output-device") &&
        imported.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D && imported.Width==d.Width &&
        imported.Height==d.Height && imported.DepthOrArraySize==1 && imported.MipLevels==1 &&
        imported.Format==d.Format && imported.SampleDesc.Count==1 && !imported.SampleDesc.Quality &&
        (imported.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
}
ID3D11Texture2D* PreparedSrOutput::TextureFor(const Owner& o,ID3D11DeviceContext* context,
    UINT width,UINT height,DXGI_FORMAT format) const noexcept {
    return o.generation>owner_.generation && Authenticate(o,context,texture_.Get(),width,height,format)?texture_.Get():nullptr;
}
Lease::Lease(std::unique_ptr<Storage> s):storage_(std::move(s)){}
Lease::~Lease(){
    if(storage_ && storage_->observation.owner.thread==GetCurrentThreadId())
        FinishOutputFlush(storage_->observation.owner,OutputFlushReason::Revoke);
    Retain(std::move(storage_));
}
std::shared_ptr<Lease> Lease::Begin(const Owner& owner,ID3D11DeviceContext* context,
    const GpuWorkDomain& workDomain,ID3D12CommandQueue* queue,
    const std::array<ID3D11Resource*,4>& resources,const void* parameters,
    std::shared_ptr<PreparedSrInputs> preparedInputs) {
    GenshinCpuStageTiming::Scope cpu(GenshinCpuStageTiming::Stage::BridgeTransportBegin); GenshinHookCpuTiming::Scope r448TransportBegin(GenshinHookCpuTiming::Stage::TransportBegin);
    // Prior outputs already have Wait/copy/retire ordered before game RTV
    // readers. The normal input Flush or pure-SR Copyback submits them too.
    {GenshinCpuStageTiming::Scope collect(GenshinCpuStageTiming::Stage::BridgeCollect);CollectRetired();}
    if(!Valid(owner) || !context || !queue || !parameters ||
       owner.context!=reinterpret_cast<uint64_t>(context) || context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE ||
       queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT)return {};
    GpuWitness queueWitness;
    if(!workDomain.BoundQueue(queue,queueWitness))return {};
    auto s=std::make_unique<Storage>();
    s->workDomain=workDomain;s->device12=workDomain.Creator();
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if(owner.device!=reinterpret_cast<uint64_t>(device.Get()) ||
       FAILED(device.As(&s->device11)) || FAILED(context->QueryInterface(IID_PPV_ARGS(&s->context))) ||
       !s->device12 || !s->workDomain.PublicOwner())return {};
    s->queue=queue;
    ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC desc{};
    if(FAILED(device.As(&dxgi)) || FAILED(dxgi->GetAdapter(&adapter)) || FAILED(adapter->GetDesc(&desc)))return {};
    const auto luid=s->device12->GetAdapterLuid();
    if(luid.HighPart!=desc.AdapterLuid.HighPart || luid.LowPart!=desc.AdapterLuid.LowPart || !s->Alive())return {};
    s->observation.owner=owner;s->observation.queue=reinterpret_cast<uint64_t>(queue);
    s->observation.parameters=reinterpret_cast<uint64_t>(parameters);
    // Validate ALL source identities/descriptors before taking a cached set or issuing GPU work.
    {GenshinCpuStageTiming::Scope surfaces(GenshinCpuStageTiming::Stage::BridgeSurfaces);
    std::array<D3D11_TEXTURE2D_DESC,4> descriptions{};
    for(size_t i=0;i<resources.size();++i){
        ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11Device> sourceDevice;auto& d=descriptions[i];
        if(!resources[i] || FAILED(resources[i]->QueryInterface(IID_PPV_ARGS(&texture))))return {};
        texture->GetDevice(&sourceDevice);texture->GetDesc(&d);
        if(!SameCom(sourceDevice.Get(),device.Get()) || !Compatible(d))return {};
        for(size_t j=0;j<i;++j)if(SameCom(resources[i],resources[j]))return {};
        d.Usage=D3D11_USAGE_DEFAULT;d.CPUAccessFlags=0;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
        d.MiscFlags=D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        s->sources[i]=resources[i];s->observation.source[i]=reinterpret_cast<uint64_t>(resources[i]);
        s->surfaceShape[i]={d.Width,d.Height,d.Format};
    }
    if(preparedInputs) {
        // Authenticate the exact prepare destination, owner context and formats.
        // Shared flags or equal dimensions alone never authorize zero-copy.
        if(preparedInputs->thread_!=GetCurrentThreadId() ||
           !SameCom(preparedInputs->context_.Get(),context) || !SameCom(preparedInputs->device_.Get(),device.Get()) ||
           !SameCom(preparedInputs->textures_[0].Get(),resources[1]) ||
           !SameCom(preparedInputs->textures_[1].Get(),resources[2]) ||
           descriptions[1].Format!=DXGI_FORMAT_R32_FLOAT || descriptions[2].Format!=DXGI_FORMAT_R16G16_FLOAT ||
           descriptions[1].Width!=descriptions[2].Width || descriptions[1].Height!=descriptions[2].Height)return {};
        s->preparedInputs=std::move(preparedInputs);
        s->directPrepared=s->preparedInputs->Import(s->device12.Get());
    }
    // The existing provider call signature stays intact. Only the exact active
    // synchronous producer may convey this separately authenticated output.
    // A changed work domain is refused before GPU work; never self-copy or
    // substitute a game RTV merely because pointer values happen to match.
    if(auto* scope=Scope::Current();scope && scope->preparedOutput) {
        auto output=scope->preparedOutput;
        if(scope->lease || scope->resources!=resources || scope->parameters!=parameters ||
           !GenshinGuides::SameOwner(scope->owner,owner) ||
           !output->Authenticate(owner,context,resources[3],descriptions[3].Width,
                descriptions[3].Height,descriptions[3].Format) ||
           output->domain_.Creator()!=s->workDomain.Creator() ||
           output->domain_.PublicOwner()!=s->workDomain.PublicOwner() ||
           output->domain_.Queue()!=queue)return {};
        s->preparedOutput=std::move(output);s->directOutput=true;
    }
    const bool reused=TakeSurfaces(*s);
    for(size_t i=0;i<resources.size();++i){
        if(s->directPrepared && (i==1 || i==2)) {
            s->shared[i]=s->preparedInputs->textures_[i-1];s->imported[i]=s->preparedInputs->imported_[i-1];
            continue;
        }
        if(s->directOutput && i==3) {
            s->shared[3]=s->preparedOutput->texture_;s->imported[3]=s->preparedOutput->imported_;
            continue;
        }
        if(reused)continue;
        if(FAILED(device->CreateTexture2D(&descriptions[i],nullptr,&s->shared[i])))return {};
        ComPtr<IDXGIResource1> dxgiResource;HANDLE handle=nullptr;
        if(FAILED(s->shared[i].As(&dxgiResource)) || FAILED(dxgiResource->CreateSharedHandle(nullptr,
            DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&handle)))return {};
        const auto opened=s->device12->OpenSharedHandle(handle,IID_PPV_ARGS(&s->imported[i]));CloseHandle(handle);
        if(FAILED(opened))return {};
    }
    for(size_t i=0;i<resources.size();++i){
        s->observation.shared[i]=reinterpret_cast<uint64_t>(s->shared[i].Get());
        s->observation.imported[i]=reinterpret_cast<uint64_t>(s->imported[i].Get());
    }
    }
    HRESULT hr=S_OK;
    {GenshinCpuStageTiming::Scope fences(GenshinCpuStageTiming::Stage::BridgeFences);
    if(!s->fences) {
        s->fences=std::make_shared<SlotFences>();auto& f=*s->fences;
        HANDLE input=nullptr,output=nullptr;
        if(FAILED(s->device11->CreateFence(0,D3D11_FENCE_FLAG_SHARED,IID_PPV_ARGS(&f.input11))) ||
           FAILED(f.input11->CreateSharedHandle(nullptr,GENERIC_ALL,nullptr,&input)))return {};
        hr=s->device12->OpenSharedHandle(input,IID_PPV_ARGS(&f.input12));CloseHandle(input);
        if(FAILED(hr) || FAILED(s->device12->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&f.output12))) ||
           FAILED(s->device12->CreateSharedHandle(f.output12.Get(),nullptr,GENERIC_ALL,nullptr,&output)))return {};
        hr=s->device11->OpenSharedFence(output,IID_PPV_ARGS(&f.output11));CloseHandle(output);
        if(FAILED(hr) || FAILED(s->device11->CreateFence(0,D3D11_FENCE_FLAG_NONE,IID_PPV_ARGS(&f.retire11))) ||
           FAILED(s->device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f.wait12))))return {};
    }
    if(!s->fences->Reusable())return {};
    auto& f=*s->fences;
    const auto inputValue=SlotFences::Next(f.inputCursor),waitValue=SlotFences::Next(f.waitCursor);
    const auto submittedValue=SlotFences::Next(f.outputCursor),returnedValue=SlotFences::Next(f.returnedCursor);
    if(!inputValue || !waitValue || !submittedValue || !returnedValue)return {};
    s->values=std::make_shared<const FenceValues>(inputValue,waitValue,submittedValue,returnedValue);
    // Allocate ordinary consumer retention before GPU work. Repeated references
    // to the same recording do not require another entry.
    if(!s->directPrepared || !PreparedSrOutput::PureSr())s->consumers.reserve(GpuWorkDomain::RecordingCount);
    }
    // Strict public-device admission before the first input copy/queue wait.
    GpuWitness domain;
    if(!s->workDomain.BoundQueue(queue,domain) ||
       !domain.Owns(s->workDomain.PublicOwner(),s->fences->input12.Get(),"input-fence-device") ||
       !domain.Owns(s->workDomain.PublicOwner(),s->fences->wait12.Get(),"wait-fence-device") ||
       !domain.Owns(s->workDomain.PublicOwner(),s->fences->output12.Get(),"output-fence-device")){
        TraceWitness(*s,"input-preflight",domain,s->device12.Get());return {};
    }
    for(const auto& resource:s->imported)if(!domain.Owns(s->device12.Get(),resource.Get(),"import-device")){
        TraceWitness(*s,"input-preflight",domain,s->device12.Get());return {};
    }
    // No early failure (including partial surface/fence creation) can publish a set.
    s->surfaceSetReady=true;
    {GenshinCpuStageTiming::Scope order(GenshinCpuStageTiming::Stage::BridgeInputOrder);
    // Construct the lease before GPU use so every subsequent failure retains its resources.
    auto lease=std::shared_ptr<Lease>(new Lease(std::move(s)));auto& state=*lease->storage_;
    StartGpuTiming(state);
    state.inputIssued=true;
    for(size_t i=0;i<3;++i) {
        if(state.directPrepared && (i==1 || i==2))continue; // Written by the actual prepare CS.
        state.context->CopyResource(state.shared[i].Get(),state.sources[i].Get());
    }
    static std::atomic<unsigned> directReports{0};
    if(state.directPrepared && directReports.fetch_add(1,std::memory_order_relaxed)<4)try {
        LOG_INFO("R044 SR direct inputs: pid={} instance={} generation={} depth_motion_copies=0 slot_imports=cached retirement=full_transport no_performance_claim=true",
            GetCurrentProcessId(),owner.instance,owner.generation);
    }catch(...){} // Optional logging must never interrupt input fencing.
    hr=GenshinHookCpuTiming::Call(GenshinHookCpuTiming::Stage::Dx11SignalApi,[&]{return state.context->Signal(state.fences->input11.Get(),state.values->input);},GenshinHookCpuTiming::ResultKind::Hresult);state.inputFenced=SUCCEEDED(hr);
    // Exact prepared-input admission above supplies the context/device/slot proof.
    // Only the existing pure-SR route may batch this Signal with its Copyback.
    // Signal success is not submission proof: Copyback must explicitly Flush,
    // and Poll/revoke/destruction drain an invocation that exits before it.
    state.inputFlushPending=SUCCEEDED(hr) && state.directPrepared && PreparedSrOutput::PureSr() &&
        !Config::Instance()->HoYoShadeEnabled.value_or_default() &&
        Config::Instance()->GenshinControlsCurrent(owner.configEpoch);
    if(!state.inputFlushPending) {
        GenshinHookCpuTiming::Call(GenshinHookCpuTiming::Stage::InputFlush,[&]{return state.context->Flush();},GenshinHookCpuTiming::ResultKind::None);
        GenshinTransport::ObserveOutputFlush(state.context.Get());
    }
    if(FAILED(hr)){Quarantine(state);lease->Revoke();return {};}
    state.waitIssued=true;
    if(FAILED(GenshinHookCpuTiming::Call(GenshinHookCpuTiming::Stage::Dx12QueueWaitApi,[&]{return queue->Wait(state.fences->input12.Get(),state.values->input);},GenshinHookCpuTiming::ResultKind::Hresult))){Quarantine(state);lease->Revoke();return {};}
    state.waitFenced=SUCCEEDED(GenshinHookCpuTiming::Call(GenshinHookCpuTiming::Stage::Dx12SignalApi,[&]{return queue->Signal(state.fences->wait12.Get(),state.values->wait);},GenshinHookCpuTiming::ResultKind::Hresult));
    if(!state.waitFenced){Quarantine(state);lease->Revoke();return {};}
    state.observation.stage=Stage::InputsOrdered;state.observation.inputFence=state.values->input;
    Trace(state,TracePoint::Inputs,S_OK);
    return lease;
    }
}
bool Lease::Match(const Owner& o) noexcept {
    if(!storage_ || storage_->observation.revoked)return false;
    if(storage_->fences && storage_->fences->quarantined.load(std::memory_order_acquire)){Revoke();return false;}
    if(!Valid(o) || !GenshinGuides::SameOwner(o,storage_->observation.owner) || !storage_->Alive()){
        Revoke();return false;}
    return true;
}
ID3D12Resource* Lease::Resource(const Owner& owner,size_t index) noexcept {
    return Match(owner) && index<4 && storage_->observation.stage!=Stage::Complete ? storage_->imported[index].Get():nullptr;
}
Lease::Submission Lease::Execute(const Owner& owner,ID3D12CommandQueue* queue,
    ID3D12GraphicsCommandList* list,ID3D12CommandAllocator* allocator) noexcept {
    GenshinCpuStageTiming::Scope cpu(GenshinCpuStageTiming::Stage::BridgeSubmitQueue);
    auto& s=*storage_;Submission result;GpuWitness witness,retirement;
    retirement.reason="not-issued";retirement.hr=S_FALSE;
    const auto reject=[&](const char* phase) {
        result.hr=witness.hr;result.reason=witness.reason;
        TraceWitness(s,phase,witness,s.device12.Get(),&retirement);Revoke();return result;
    };
    if(s.executed){witness.Reject("duplicate-execute");return reject("preflight");}
    if(!Match(owner)){witness.Reject("owner-or-device");return reject("preflight");}
    if(s.observation.stage!=Stage::InputsOrdered){witness.Reject("stage");return reject("preflight");}
    if(queue!=s.queue.Get()){witness.Reject("queue-identity");return reject("preflight");}
    if(!s.workDomain.Commands(queue,list,allocator,s.fences->output12.Get(),witness))return reject("preflight");
    // Pin before Execute (including reentrant hooks/exceptions). From here, any
    // ambiguous execution stays retained until an actual queue fence completes.
    s.list=list;s.allocator=allocator;s.submittedQueue=queue;
    s.executed=true;s.outputFenced=false;s.lastConsumer=s.values->submitted;result.executed=true;
    if(s.gpu12Ended && (s.gpu12List!=list ||
       FAILED(queue->GetTimestampFrequency(&s.gpu12Hz)) || !s.gpu12Hz ||
       FAILED(queue->GetClockCalibration(&s.gpu12CalibrationGpu,&s.gpu12CalibrationCpu))))
        s.gpu12Hz=s.gpu12CalibrationGpu=s.gpu12CalibrationCpu=0;
    bool returned=false;
    try {ID3D12CommandList* lists[]={list};GenshinHookCpuTiming::Call(GenshinHookCpuTiming::Stage::Dx12Submit,[&]{return queue->ExecuteCommandLists(1,lists);},GenshinHookCpuTiming::ResultKind::None);returned=true;}
    catch(...){witness.Reject("execute-exception",E_FAIL);}
    result.returned=returned;
    bool valid=returned;
    if(valid && !Match(owner)){valid=false;witness.Reject("post-owner-or-device");}
    if(valid && s.observation.stage!=Stage::InputsOrdered){valid=false;witness.Reject("post-stage");}
    if(valid)valid=s.workDomain.Commands(queue,list,allocator,s.fences->output12.Get(),witness);
    // Even a post-execute witness refusal must fence the actual bound queue.
    // This certifies retirement only; it never changes failure into copyback.
    if(s.workDomain.BoundQueue(queue,retirement) &&
       retirement.Owns(s.workDomain.PublicOwner(),s.fences->output12.Get(),"retire-fence-device")){
        retirement.reason="queue-signal";
        try {retirement.hr=GenshinHookCpuTiming::Call(GenshinHookCpuTiming::Stage::Dx12SignalApi,[&]{return queue->Signal(s.fences->output12.Get(),s.lastConsumer);},GenshinHookCpuTiming::ResultKind::Hresult);}
        catch(...){retirement.hr=E_FAIL;}
        s.outputFenced=SUCCEEDED(retirement.hr);
        if(!s.outputFenced)retirement.reason="retire-queue-signal";
        Trace(s,TracePoint::OutputSignal,retirement.hr);
    }
    // Record the failure before any later consumer or completion poll can run.
    if(!s.outputFenced)Quarantine(s);
    if(!valid)return reject("post-execute");
    if(!s.outputFenced){witness=std::move(retirement);return reject("gpu-fence");}
    s.observation.stage=Stage::Submitted;
    s.observation.submissionFence=s.values->submitted;s.observation.consumerFence=s.lastConsumer;
    result.submitted=true;return result;
}
void Lease::BeginGpuTimeline(const Owner& owner,ID3D12GraphicsCommandList* list,
    ID3D12CommandAllocator* allocator) noexcept {
    if(!storage_ || !GpuTimeline::Enabled())return;
    auto& s=*storage_;
    if(!s.gpuTimingSampleMs || s.gpu12Started || s.executed || s.observation.revoked ||
       !Valid(owner) || !GenshinGuides::SameOwner(owner,s.observation.owner) ||
       s.observation.stage!=Stage::InputsOrdered)return;
    const auto marker=GpuTimeline::Capture(ReflexHooks::lastMarkerFrameId(RENDERSUBMIT_START));
    GpuWitness witness;
    if(!marker.frame || !s.workDomain.Commands(s.queue.Get(),list,allocator,s.fences->output12.Get(),witness))return;
    D3D12_QUERY_HEAP_DESC query{D3D12_QUERY_HEAP_TYPE_TIMESTAMP,2,0};
    if(FAILED(s.device12->CreateQueryHeap(&query,IID_PPV_ARGS(&s.gpu12Queries))))return;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=2*sizeof(uint64_t);
    desc.Height=desc.DepthOrArraySize=desc.MipLevels=desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if(FAILED(s.device12->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
        D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&s.gpu12Readback)))){
        s.gpu12Queries.Reset();return;
    }
    s.gpu12Frame=marker.frame;s.gpu12MarkerDevice=marker.device;s.gpu12List=list;
    list->EndQuery(s.gpu12Queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);s.gpu12Started=true;
}
void Lease::EndGpuTimeline(const Owner& owner,ID3D12GraphicsCommandList* list) noexcept {
    if(!storage_)return;
    auto& s=*storage_;
    if(!s.gpu12Started || s.gpu12Ended || s.gpu12List!=list ||
       !Valid(owner) || !GenshinGuides::SameOwner(owner,s.observation.owner))return;
    list->EndQuery(s.gpu12Queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);
    list->ResolveQueryData(s.gpu12Queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,s.gpu12Readback.Get(),0);
    s.gpu12Ended=true;
}
bool Lease::Restored(const Owner& owner,bool parameters,bool pipeline) noexcept {
    if(!Match(owner))return false;
    storage_->observation.parametersRestored=parameters;storage_->observation.pipelineRestored=pipeline;
    if(!parameters || !pipeline){Revoke();return false;}return true;
}
bool Lease::Copyback(const Owner& owner,bool deferFlush) noexcept {
    GenshinCpuStageTiming::Scope cpu(GenshinCpuStageTiming::Stage::BridgeCopyback); GenshinHookCpuTiming::Scope r448TransportCopyback(GenshinHookCpuTiming::Stage::TransportCopyback);
    GenshinSubstage::Event(storage_->substageKey,GenshinSubstage::Stage::OutputCopyback,GenshinSubstage::State::Running);
    if(!Match(owner))return false;auto& s=*storage_;
    if(s.observation.stage!=Stage::Submitted || !s.observation.parametersRestored || !s.observation.pipelineRestored){Revoke();return false;}
    const auto copybackWaitValue=s.lastConsumer;
    const auto wait=GenshinHookCpuTiming::Call(GenshinHookCpuTiming::Stage::Dx11WaitApi,[&]{return s.context->Wait(s.fences->output11.Get(),copybackWaitValue);},GenshinHookCpuTiming::ResultKind::Hresult);Trace(s,TracePoint::CopybackWait,wait);
    if(FAILED(wait)){
        Quarantine(s);
        GenshinSubstage::Event(s.substageKey,GenshinSubstage::Stage::OutputCopyback,GenshinSubstage::State::Failed,
            GenshinSubstage::Resource::Output,GenshinSubstage::Domain::Hresult,wait);
        Revoke();return false;}
    s.copybackWaitValue=copybackWaitValue; // Only after the real Wait succeeded.
    if(s.directOutput && (!s.preparedOutput ||
       s.sources[3].Get()!=s.preparedOutput->texture_.Get() ||
       s.shared[3].Get()!=s.preparedOutput->texture_.Get() ||
       s.imported[3].Get()!=s.preparedOutput->imported_.Get())) {
        Quarantine(s);Revoke();return false;
    }
    // copyIssued covers the D3D11 output handoff even when no copy is needed.
    // Keep Wait/copy before Legacy's immediate h.output -> game RTV copy.
    // Only the explicit Flush may move; CPU ordering is not GPU completion.
    s.copyIssued=true;
    if(!s.directOutput)s.context->CopyResource(s.sources[3].Get(),s.shared[3].Get());
    static std::array<std::atomic<unsigned>,2> outputReports{};
    auto& reports=outputReports[s.directOutput?0:1];auto seen=reports.load(std::memory_order_relaxed);
    while(seen<3 && !reports.compare_exchange_weak(seen,seen+1,std::memory_order_relaxed)){}
    if(seen<3)try {
        LOG_INFO("R047 SR output: pid={} instance={} generation={} direct_output={} shared_to_history_copies={} final_game_copy=unchanged retirement=full_transport no_performance_claim=true",
            GetCurrentProcessId(),owner.instance,owner.generation,s.directOutput,s.directOutput?0:1);
    }catch(...){}
    const auto signal=GenshinHookCpuTiming::Call(GenshinHookCpuTiming::Stage::Dx11SignalApi,[&]{return s.context->Signal(s.fences->retire11.Get(),s.values->returned);},GenshinHookCpuTiming::ResultKind::Hresult);s.copyFenced=SUCCEEDED(signal);
    FinishGpuTiming(s);
    const auto outputRoute=CurrentOutputRoute();
    s.batchAtNativeAcquire=deferFlush && outputRoute.batchAtNativeAcquire;
    s.outputFilterStage=outputRoute.filterStage;
    s.outputFlushPending=true;s.outputFlushTick=GetTickCount64();
    s.observation.outputFlushPending=true;
    // Submit the complete input Signal -> DX12 Wait/Execute -> DX11 Wait/copy
    // chain before returning. A later recording-slot CPU wait must never depend
    // on an input left buffered until Present or the next invocation's Begin.
    if((s.inputFlushPending || !deferFlush || !s.copyFenced) &&
       !FinishOutputFlush(owner,OutputFlushReason::Consumer)){
        Quarantine(s);Revoke();return false;
    }
    Trace(s,TracePoint::CopybackSignal,signal);
    if(!s.copyFenced){
        Quarantine(s);
        GenshinSubstage::Event(s.substageKey,GenshinSubstage::Stage::OutputCopyback,GenshinSubstage::State::Failed,
            GenshinSubstage::Resource::Output,GenshinSubstage::Domain::Hresult,signal);
        Revoke();return false;}
    s.observation.copyback=true;s.observation.stage=Stage::CopybackOrdered;return true;
}
bool Lease::OutputFlushPending() const noexcept {return storage_ && storage_->outputFlushPending;}
bool Lease::CanBatchOutputFlush() const noexcept {
    if(!storage_ || !storage_->batchAtNativeAcquire)return false;
    const auto route=CurrentOutputRoute();
    return route.batchAtNativeAcquire && route.filterStage==storage_->outputFilterStage;
}
bool Lease::ObserveOutputFlush(const Owner& owner,ID3D11DeviceContext* context) noexcept {
    if(!storage_ || owner.thread!=GetCurrentThreadId() ||
       !GenshinGuides::SameOwner(owner,storage_->observation.owner) ||
       (context!=storage_->context.Get() && !SameCom(context,storage_->context.Get())) || !storage_->Alive())return false;
    auto& s=*storage_;
    s.inputFlushPending=false;
    if(s.outputFlushPending) {
        s.outputFlushPending=false;s.outputFlushIssued=true;
        s.observation.outputFlushPending=false;s.observation.outputFlushIssued=true;
    }
    return true; // the caller already flushed; fence completion is unchanged
}
bool Lease::FinishOutputFlush(const Owner& owner,OutputFlushReason) noexcept {
    if(!storage_ || owner.thread!=GetCurrentThreadId() ||
       !GenshinGuides::SameOwner(owner,storage_->observation.owner))return false;
    auto& s=*storage_;
    if(!s.inputFlushPending && !s.outputFlushPending)return true;
    if(!s.context || !s.Alive())return false;
    GenshinHookCpuTiming::Call((s.inputFlushPending?GenshinHookCpuTiming::Stage::DeferredInputOutputFlush:GenshinHookCpuTiming::Stage::OutputFlush),[&]{return s.context->Flush();},GenshinHookCpuTiming::ResultKind::None);
    s.inputFlushPending=false;
    // An early input-only drain must not invent an output submission receipt.
    if(s.outputFlushPending) {
        s.outputFlushPending=false;s.outputFlushIssued=true;
        s.observation.outputFlushPending=false;s.observation.outputFlushIssued=true;
    }
    // One Flush submits every preceding command on this exact immediate
    // context, including other instances' pending outputs. Mark only that
    // actually submitted group; each lease still polls its own retire fence.
    GenshinTransport::ObserveOutputFlush(s.context.Get());
    return true; // submission only; Done/Poll still require real fences
}
OutputRouteContract CurrentOutputRoute() noexcept {
    OutputRouteContract route;
    const auto& cfg=*Config::Instance();
    const bool beforeSr=HoYoShade::Selected(HoYoShade::Stage::BeforeSr);
    const bool beforeNr=HoYoShade::Selected(HoYoShade::Stage::BeforeNr);
    const bool finalOutput=HoYoShade::Selected(HoYoShade::Stage::FinalOutput);
    const bool requested=cfg.HoYoShadeEnabled.value_or_default();
    const bool filter=requested || beforeSr || beforeNr || finalOutput;
    route.filterStage=filter?OutputFilterStage::Unknown:OutputFilterStage::None;
    // Selected checks the existing coherent desired/applied settings and
    // connecting state. An enabled but unresolved stage keeps synchronous IO.
    // No runtime/cache scan or new resource authority is needed for scheduling.
    if(requested) {
        const auto selectedStage=cfg.HoYoShadeStage.value_or_default();
        if(selectedStage==1 && beforeSr && !beforeNr && !finalOutput)
            route.filterStage=OutputFilterStage::BeforeSr;
        else if(selectedStage==2 && beforeNr && !beforeSr && !finalOutput)
            route.filterStage=OutputFilterStage::BeforeNr;
        else if(selectedStage==3 && finalOutput && !beforeSr && !beforeNr)
            route.filterStage=OutputFilterStage::FinalOutput;
    }
    const auto& state=State::Instance();
    // Scope/actual DLSS backend are checked by the existing producer callsite.
    // No setting is forced: changed placement/consumer/provider returns to sync.
    // BeforeSR: RenderStage submits on the SR queue before provider Execute.
    // BeforeNR/FinalOutput: NativeProducer invokes them after Acquire's Flush.
    // Their own queue/fence, uncertainty and color/depth gates stay unchanged.
    if(route.filterStage!=OutputFilterStage::Unknown &&
       GenshinFgConsumer::NewWorkRequested() && cfg.DlssNrEnabled.value_or_default() &&
       cfg.DlssNrFinishedPicture.value_or_default() && !cfg.DlssNrRunBeforeSr.value_or_default() &&
       !cfg.DlssNrDeferredDlss.value_or_default() && !native::OpticalFgNrEnabled() &&
       state.activeFgInput==FGInput::Upscaler && state.activeFgOutput==FGOutput::DLSSG &&
       state.swapchainInteropApi==SwapchainInteropApi::Dx11wDx12 &&
       dynamic_cast<DLSSG_Dx12*>(state.currentFG)) {
        route.deferOutputFlush=true;route.batchAtNativeAcquire=true;
        route.reason=filter?"genshin-filter-fullstack-acquire-submit":"genshin-fullstack-acquire-submit";return route;
    }
    if(GenshinFgConsumer::HandoffRequired()){route.reason="fg-synchronous-handoff";return route;}
    if(cfg.DlssNrEnabled.value_or_default() || native::OpticalFgNrEnabled()){
        route.reason="nr-synchronous-handoff";return route;
    }
    if(filter){
        route.reason="filter-synchronous-handoff";return route;
    }
    route.deferOutputFlush=true;route.reason="sr-output-flush-deferred";return route;
}
bool QueueDeferredOutputFlush(const std::shared_ptr<Lease>& lease,const Owner& owner) noexcept {
    if(!lease || owner.thread!=GetCurrentThreadId())return false;
    if(!lease->OutputFlushPending())return true;
    for(auto& pending:pendingOutputs)if(pending.lease && !pending.lease->OutputFlushPending())pending={};
    for(auto& pending:pendingOutputs)if(pending.lease==lease)return true;
    for(auto& pending:pendingOutputs)if(!pending.lease){
        pending.lease=lease;pending.owner=owner;
        pending.device=reinterpret_cast<ID3D11Device*>(owner.device);return true;
    }
    // Capacity pressure submits rather than accumulating more deferred work.
    FinishPendingOutput(nullptr,OutputFlushReason::ResourceReuse);
    return lease->FinishOutputFlush(owner,OutputFlushReason::ResourceReuse);
}
size_t FinishPendingOutput(ID3D11Device* device,OutputFlushReason reason) noexcept {
    size_t remaining=0;
    for(auto& pending:pendingOutputs){
        if(!pending.lease)continue;
        if(device && pending.device!=device && !SameCom(device,pending.device)){++remaining;continue;}
        auto lease=std::move(pending.lease);const auto owner=pending.owner;pending={};
        // A failed device retains its actual owners through Lease destruction.
        if(!lease->FinishOutputFlush(owner,reason)){lease->Revoke();++remaining;}
    }
    return remaining;
}
void ObserveOutputFlush(ID3D11DeviceContext* context) noexcept {
    if(!context)return;
    for(auto& pending:pendingOutputs){
        if(!pending.lease || !pending.lease->ObserveOutputFlush(pending.owner,context))continue;
        // Drop the handoff reference only after the actual matching submission.
        // Escaped packets/real fence retirement still own the GPU work.
        pending={};
    }
}
PresentOutputScope::PresentOutputScope(bool active) noexcept:active_(active) {
    if(!active_)return;
    for(auto& pending:pendingOutputs){
        if(!pending.lease || pending.lease->CanBatchOutputFlush())continue;
        auto lease=std::move(pending.lease);const auto owner=pending.owner;pending={};
        if(!lease->FinishOutputFlush(owner,OutputFlushReason::Present))lease->Revoke();
    }
}
PresentOutputScope::~PresentOutputScope() {
    if(active_)FinishPendingOutput(nullptr,OutputFlushReason::Consumer);
}
bool Lease::ConsumerSubmitted(const Owner& owner,ID3D12CommandQueue* queue,ID3D12GraphicsCommandList* list,
    ID3D12CommandAllocator* allocator) {
    auto& s=*storage_;
    FinishOutputFlush(owner,OutputFlushReason::Consumer);
    // Called after the real submission. Pin every call before admission, even
    // a later rejected call, so an earlier failure's owners are not overwritten.
    bool pinned=false;
    for(const auto& work:s.consumers)
        if(work.list.Get()==list && work.allocator.Get()==allocator && work.queue.Get()==queue){pinned=true;break;}
    try {if(!pinned)s.consumers.push_back({list,allocator,queue});}
    catch (...) {
        // No allocation is required to retain the first overflow owner. If an
        // already revoked caller submits yet again during allocation failure,
        // preserve the previous emergency COM references permanently as well.
        // There is no safe retirement proof for either submission.
        (void)s.rejectedQueue.Detach();(void)s.rejectedConsumer.list.Detach();
        (void)s.rejectedConsumer.allocator.Detach();(void)s.rejectedConsumer.queue.Detach();
        s.rejectedQueue=queue;s.rejectedConsumer={list,allocator,queue};
        s.outputFenced=false;s.executed=true;Quarantine(s);Revoke();return false;
    }
    GpuWitness witness;
    if(!Match(owner) || s.directOutput || queue!=s.queue.Get() ||
       !s.workDomain.Commands(queue,list,allocator,s.fences->output12.Get(),witness) ||
       (s.observation.stage!=Stage::Submitted && s.observation.stage!=Stage::CopybackOrdered)){
        s.outputFenced=false;s.executed=true;Quarantine(s);Revoke();return false;}
    // Reserve before Signal; overflow after submitted work is uncertain GPU
    // ownership and must retain the work rather than wrap to 0 or UINT64_MAX.
    const auto value=SlotFences::Next(s.fences->outputCursor);
    if(!value){s.outputFenced=false;Quarantine(s);Revoke();return false;}
    s.lastConsumer=value;s.outputFenced=false;
    if(s.surfaceLifetime) {
        auto* life=const_cast<ExportedSurfaces*>(static_cast<const ExportedSurfaces*>(s.surfaceLifetime.get()));
        life->outputFenced.store(false,std::memory_order_release);
        life->lastConsumer.store(value,std::memory_order_release);
    }
    HRESULT hr=E_FAIL;
    try {hr=GenshinHookCpuTiming::Call(GenshinHookCpuTiming::Stage::Dx12SignalApi,[&]{return queue->Signal(s.fences->output12.Get(),value);},GenshinHookCpuTiming::ResultKind::Hresult);}catch(...){}
    s.outputFenced=SUCCEEDED(hr);
    if(!s.outputFenced){Quarantine(s);Revoke();return false;}
    if(s.surfaceLifetime) {
        auto* life=const_cast<ExportedSurfaces*>(static_cast<const ExportedSurfaces*>(s.surfaceLifetime.get()));
        life->outputFenced.store(true,std::memory_order_release);
    }
    s.observation.consumerFence=value;return true;
}
void Lease::Revoke() noexcept {
    if(storage_){
        FinishOutputFlush(storage_->observation.owner,OutputFlushReason::Revoke);
        storage_->observation.revoked=true;storage_->observation.stage=Stage::Revoked;
    }
}
bool Lease::ObservationNoEscapedReaders(const Owner& owner) const noexcept {
    if(!storage_ || !Valid(owner) || owner.thread!=GetCurrentThreadId())return false;
    const auto& s=*storage_;
    return SameOwner(owner,s.observation.owner) && !s.observation.revoked && s.Alive() &&
        s.fences && !s.fences->quarantined.load(std::memory_order_acquire) && !s.escaped && !s.surfaceLifetime;
}
bool Lease::ObservationQuiescent(const Owner& owner) const noexcept {
    return ObservationNoEscapedReaders(owner) && storage_->observation.stage==Stage::Complete && storage_->Done();
}
bool Lease::Poll() noexcept {
    GenshinCpuStageTiming::Scope cpu(GenshinCpuStageTiming::Stage::BridgePoll); GenshinHookCpuTiming::Scope r448TransportPoll(GenshinHookCpuTiming::Stage::TransportPoll);
    if(!storage_)return false;
    if(!storage_->Alive()){Revoke();return false;}
    auto& s=*storage_;
    // A failed/abandoned provider may reach Poll without Copyback. Do not leave
    // its queue Wait dependent on a future invocation's recording-slot wait.
    if(s.inputFlushPending && !FinishOutputFlush(s.observation.owner,OutputFlushReason::ResourceReuse)){
        Quarantine(s);Revoke();return false;
    }
    // No-Present progress uses the existing owner Poll, at most one submission
    // per pending output after the grace period, never a Flush per draw.
    if(s.outputFlushPending && GetTickCount64()-s.outputFlushTick>=100)
        FinishOutputFlush(s.observation.owner,OutputFlushReason::ResourceReuse);
    s.observation.inputsComplete=s.inputFenced && Reached(s.fences->input11.Get(),s.values->input);
    s.observation.inputWaitComplete=s.waitFenced && Reached(s.fences->wait12.Get(),s.values->wait);
    s.observation.consumerComplete=s.executed && s.outputFenced &&
        !s.fences->quarantined.load(std::memory_order_acquire) && Reached(s.fences->output12.Get(),s.lastConsumer);
    s.observation.copybackComplete=s.copyIssued && s.copyFenced && Reached(s.fences->retire11.Get(),s.values->returned);
    // Monitor queries may complete before later NR/FG readers. Their own S_OK
    // completion permits publication; it never retires transport/consumer owners.
    LogGpuTiming(s);
    if(!storage_->Done()){Trace(s,TracePoint::Pending,S_FALSE);return false;}
    if(s.observation.copybackComplete && !s.observation.revoked)
        GenshinSubstage::Event(s.substageKey,GenshinSubstage::Stage::OutputCopyback,
            GenshinSubstage::State::Succeeded,GenshinSubstage::Resource::Output);
    Trace(s,TracePoint::Complete,S_OK);
    if(storage_->observation.copyback && !storage_->observation.revoked)storage_->observation.stage=Stage::Complete;
    return true;
}
Observation Lease::Snapshot()const noexcept{return storage_?storage_->observation:Observation{};}
void Lease::LogFailureSnapshot(UINT recordingSlot) const noexcept {
    if(!storage_ || storage_->observation.owner.thread!=GetCurrentThreadId())return;
    const auto lastError=GetLastError();
    try {
        const auto& s=*storage_;const auto& o=s.observation;
        const auto read=[](const auto& fence) {return fence?fence->GetCompletedValue():UINT64_MAX;};
        const auto inputDone=s.fences?read(s.fences->input11):UINT64_MAX;
        const auto waitDone=s.fences?read(s.fences->wait12):UINT64_MAX;
        const auto outputDone=s.fences?read(s.fences->output12):UINT64_MAX;
        const auto copyDone=s.fences?read(s.fences->retire11):UINT64_MAX;
        LOG_WARN("R1068 SR lease failure: pid={} slot={} instance={} generation={} source_frame={} stage={} revoked={} "
            "creator={} public_owner={} queue={} submitted_queue={} list={} allocator={} "
            "input_fence={} input_value={} input_done={} input_issued={} input_signalled={} "
            "wait_fence={} wait_value={} wait_done={} wait_issued={} wait_signalled={} "
            "output_fence={} submitted_value={} consumer_value={} output_done={} executed={} output_signalled={} "
            "copy_fence={} copy_value={} copy_done={} copy_issued={} copy_signalled={} copy_wait_value={} "
            "input_flush_pending={} output_flush_pending={} quarantined={} direct_inputs={} direct_output={} "
            "escaped={} external_lifetime={} owner_thread={} thread={} max_is_not_complete=true observation_only=true",
            GetCurrentProcessId(),recordingSlot,o.owner.instance,o.owner.generation,o.owner.sourceFrame,
            static_cast<unsigned>(o.stage),o.revoked,reinterpret_cast<uint64_t>(s.device12.Get()),
            reinterpret_cast<uint64_t>(s.workDomain.PublicOwner()),o.queue,
            reinterpret_cast<uint64_t>(s.submittedQueue.Get()),reinterpret_cast<uint64_t>(s.list.Get()),
            reinterpret_cast<uint64_t>(s.allocator.Get()),
            s.fences?reinterpret_cast<uint64_t>(s.fences->input11.Get()):0,s.values?s.values->input:0,inputDone,s.inputIssued,s.inputFenced,
            s.fences?reinterpret_cast<uint64_t>(s.fences->wait12.Get()):0,s.values?s.values->wait:0,waitDone,s.waitIssued,s.waitFenced,
            s.fences?reinterpret_cast<uint64_t>(s.fences->output12.Get()):0,s.values?s.values->submitted:0,
            s.lastConsumer,outputDone,s.executed,s.outputFenced,
            s.fences?reinterpret_cast<uint64_t>(s.fences->retire11.Get()):0,s.values?s.values->returned:0,
            copyDone,s.copyIssued,s.copyFenced,s.copybackWaitValue,s.inputFlushPending,s.outputFlushPending,
            s.fences&&s.fences->quarantined.load(std::memory_order_acquire),s.directPrepared,s.directOutput,
            s.escaped,bool(s.surfaceLifetime),o.owner.thread,GetCurrentThreadId());
        for(size_t i=0;i<s.imported.size();++i)if(s.imported[i]) {
            const auto d=s.imported[i]->GetDesc();
            LOG_WARN("R1068 SR lease resource: pid={} slot={} generation={} index={} source={} shared={} imported={} "
                "width={} height={} format={} flags={} state=UNKNOWN barrier_identity=UNKNOWN observation_only=true",
                GetCurrentProcessId(),recordingSlot,o.owner.generation,i,o.source[i],o.shared[i],o.imported[i],
                d.Width,d.Height,static_cast<unsigned>(d.Format),static_cast<unsigned>(d.Flags));
        }
    }catch(...){} // A diagnostic failure cannot revoke, flush, or retire a lease.
    SetLastError(lastError);
}
std::shared_ptr<PreparedSrOutput> Lease::PrepareSrOutput(const Owner& owner) noexcept {
    if(!FinishOutputFlush(owner,OutputFlushReason::ResourceReuse))return {};
    if(!Match(owner) || !PreparedSrOutput::PureSr() || !Poll())return {};
    auto& s=*storage_;
    if(s.observation.stage!=Stage::Complete || s.escaped || s.surfaceLifetime || !s.Done())return {};
    try {
        ComPtr<ID3D11Texture2D> source;
        if(FAILED(s.sources[3].As(&source)))return {};
        D3D11_TEXTURE2D_DESC d{};source->GetDesc(&d);
        // The initial candidate covers the two existing exact-format SR output
        // shapes. Other formats keep the original private/shared transport.
        if(!Compatible(d) || (d.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT &&
            d.Format!=DXGI_FORMAT_R8G8B8A8_UNORM))return {};
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};support.Format=d.Format;
        if(FAILED(s.device12->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof(support))) ||
           !(support.Support1&D3D12_FORMAT_SUPPORT1_TEXTURE2D) ||
           !(support.Support1&D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) ||
           !(support.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))return {};
        auto result=std::shared_ptr<PreparedSrOutput>(new PreparedSrOutput);
        result->owner_=owner;result->context_=s.context;result->device_=s.device11;result->domain_=s.workDomain;
        d.Usage=D3D11_USAGE_DEFAULT;d.CPUAccessFlags=0;
        d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
        d.MiscFlags=D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        result->desc_=d;
        if(FAILED(s.device11->CreateTexture2D(&d,nullptr,&result->texture_)) ||
           FAILED(s.device11->CreateUnorderedAccessView(result->texture_.Get(),nullptr,&result->uav_)))return {};
        ComPtr<IDXGIResource1> resource;HANDLE handle=nullptr;
        if(FAILED(result->texture_.As(&resource)) || FAILED(resource->CreateSharedHandle(nullptr,
            DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&handle)))return {};
        const auto opened=s.device12->OpenSharedHandle(handle,IID_PPV_ARGS(&result->imported_));CloseHandle(handle);
        if(FAILED(opened))return {};
        // This creates no new frame receipt. Later selection and admission
        // require the actual later owner/generation and immutable guide facts.
        if(!result->Authenticate(owner,result->context_.Get(),result->texture_.Get(),d.Width,d.Height,d.Format))return {};
        return result;
    }catch(...) {return {};}
}
std::shared_ptr<const CompletedTransfer> Lease::Completed(const Owner& owner) noexcept {
    if(!FinishOutputFlush(owner,OutputFlushReason::Consumer))return {};
    // An exported packet pins its actual prepare slot, including consumers
    // toggled on after SR. Direct output still has no immutable export contract.
    if(!Match(owner) || storage_->directOutput || !Poll())return {};
    auto& s=*storage_;const auto& o=s.observation;
    if(o.revoked || o.stage!=Stage::Complete || !o.parametersRestored || !o.pipelineRestored || !o.copyback ||
       !o.inputsComplete || !o.inputWaitComplete || !o.consumerComplete || !o.copybackComplete ||
       !s.inputIssued || !s.waitIssued || !s.executed || !s.copyIssued || !s.lastConsumer ||
       !s.list || !s.allocator || !SameCom(s.queue.Get(),s.submittedQueue.Get()) ||
       !s.fences->input12 || !s.fences->output12 || !s.fences->retire11 || !s.Done())return {};
    try {
        auto result=std::shared_ptr<CompletedTransfer>(new CompletedTransfer);
        auto& v=result->view_;v.owner=owner;v.queue=s.submittedQueue;v.list=s.list;v.allocator=s.allocator;
        v.sources=s.sources;v.imported=s.imported;v.inputs=s.fences->input12;v.submitted=s.fences->output12;v.returned=s.fences->retire11;
        v.surfaceLifetime=ExportLifetime(s);
        if(!v.surfaceLifetime)return {};
        v.inputValue=s.values->input;v.submittedValue=s.lastConsumer;v.returnedValue=s.values->returned;
        for(size_t i=0;i<v.sources.size();++i){
            ComPtr<ID3D11Texture2D> texture;
            if(!v.sources[i] || !v.imported[i] || FAILED(v.sources[i].As(&texture)))return {};
            texture->GetDesc(&v.descriptors[i]);
            if(!Compatible(v.descriptors[i]))return {};
        }
        s.escaped=true;TraceSurfaces(s,SurfacePoint::CompletedEscape);
        return result;
    }catch(const std::bad_alloc&){return {};}
}
bool QueuedTransfer::CopybackOrdersRead(ID3D11DeviceContext4* context)const noexcept {
    if(!context || view_.owner.thread!=GetCurrentThreadId() || !context_ || !submitted11_ ||
       !view_.submittedValue || view_.submittedValue==UINT64_MAX ||
       !copybackWaitValue_ || copybackWaitValue_==UINT64_MAX || copybackWaitValue_!=view_.submittedValue ||
       context!=context_.Get())return false;
    ComPtr<ID3D11Device> device;context->GetDevice(&device);
    return device && SUCCEEDED(device->GetDeviceRemovedReason());
}
bool QueuedTransfer::OrderRead(ID3D11DeviceContext4* context)const noexcept {
    if(!context || view_.owner.thread!=GetCurrentThreadId() || !context_ || !submitted11_ ||
       !view_.submittedValue || view_.submittedValue==UINT64_MAX || !SameCom(context,context_.Get()))return false;
    ComPtr<ID3D11Device> device;context->GetDevice(&device);
    if(!device || FAILED(device->GetDeviceRemovedReason()))return false;
    // Copyback already ordered this exact context after this packet's fence.
    // Later reads remain behind it without another identical GPU queue Wait.
    // A post-copyback consumer can raise submittedValue: keep the real Wait
    // whenever the immutable receipt does not cover that newer dependency.
    if(copybackWaitValue_ && copybackWaitValue_!=UINT64_MAX && copybackWaitValue_>=view_.submittedValue)return true;
    return SUCCEEDED(GenshinHookCpuTiming::Call(GenshinHookCpuTiming::Stage::Dx11WaitApi,[&]{return context->Wait(submitted11_.Get(),view_.submittedValue);},GenshinHookCpuTiming::ResultKind::Hresult));
}
std::shared_ptr<const QueuedTransfer> Lease::Queued(const Owner& owner) noexcept {
    // These packets read the already submitted SR inputs/output12 fence, not
    // h.output's pending D3D11 return. Completed exports still finish eagerly.
    if(!CanBatchOutputFlush() && !FinishOutputFlush(owner,OutputFlushReason::Consumer))return {};
    if(!Match(owner) || storage_->directOutput)return {};
    auto& s=*storage_;const auto& o=s.observation;
    if(o.revoked || (o.stage!=Stage::CopybackOrdered && o.stage!=Stage::Complete) ||
       !o.parametersRestored || !o.pipelineRestored || !o.copyback ||
       !s.inputIssued || !s.inputFenced || !s.waitIssued || !s.waitFenced ||
       !s.executed || !s.outputFenced || !s.copyIssued || !s.copyFenced || !s.lastConsumer ||
       !s.list || !s.allocator || !SameCom(s.queue.Get(),s.submittedQueue.Get()) ||
       !s.context || !s.fences->input12 || !s.fences->output12 || !s.fences->output11 || !s.fences->retire11)return {};
    try {
        auto result=std::shared_ptr<QueuedTransfer>(new QueuedTransfer);
        auto& v=result->view_;v.owner=owner;v.queue=s.submittedQueue;v.list=s.list;v.allocator=s.allocator;
        v.sources=s.sources;v.imported=s.imported;v.inputs=s.fences->input12;v.submitted=s.fences->output12;v.returned=s.fences->retire11;
        v.surfaceLifetime=ExportLifetime(s);
        if(!v.surfaceLifetime)return {};
        v.inputValue=s.values->input;v.submittedValue=s.lastConsumer;v.returnedValue=s.values->returned;
        result->prepared_=s.shared;result->context_=s.context;result->submitted11_=s.fences->output11;
        // Match(owner) bound this Storage generation; Copyback used these same
        // retained context/fence objects. A new Storage starts with no receipt.
        result->copybackWaitValue_=s.copybackWaitValue;
        for(size_t i=0;i<v.sources.size();++i){
            ComPtr<ID3D11Texture2D> texture;D3D11_TEXTURE2D_DESC prepared{};
            if(!v.sources[i] || !v.imported[i] || !s.shared[i] || FAILED(v.sources[i].As(&texture)))return {};
            texture->GetDesc(&v.descriptors[i]);s.shared[i]->GetDesc(&prepared);
            const auto& d=v.descriptors[i];
            // Only Begin's authenticated prepared depth/MV pair may alias its
            // source. Its physical slot is pinned above; every other surface
            // keeps the original distinct-copy requirement.
            const bool directInput=s.directPrepared && (i==1 || i==2);
            if(!Compatible(d) || prepared.Width!=d.Width || prepared.Height!=d.Height ||
               prepared.Format!=d.Format || prepared.MipLevels!=1 || prepared.ArraySize!=1 ||
               prepared.SampleDesc.Count!=1 || prepared.SampleDesc.Quality ||
               SameCom(texture.Get(),s.shared[i].Get())!=directInput)return {};
        }
        s.escaped=true;TraceSurfaces(s,SurfacePoint::QueuedEscape);
        return result;
    }catch(const std::bad_alloc&){return {};}
}
std::shared_ptr<const CompletedTransfer> Lease::CompletedDepth(const Owner& owner,
    ID3D11DeviceContext* context,ID3D11Resource* source) noexcept {
    if(!FinishOutputFlush(owner,OutputFlushReason::Consumer))return {};
    if(!Match(owner) || storage_->directOutput || !context || !source ||
       owner.context!=reinterpret_cast<uint64_t>(context) || context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE ||
       !SameCom(context,storage_->context.Get()) || !SameCom(source,storage_->sources[1].Get()))return {};
    if(auto completed=Completed(owner))return completed;
    auto& s=*storage_;const auto& o=s.observation;
    // An incomplete Begin, failed Evaluate, missing restoration or unfenced
    // submission cannot start a wait or manufacture a native import receipt.
    if(o.revoked || o.stage!=Stage::CopybackOrdered || !o.parametersRestored || !o.pipelineRestored || !o.copyback ||
       !s.inputIssued || !s.inputFenced || !s.waitIssued || !s.waitFenced || !s.executed || !s.outputFenced ||
       !s.copyIssued || !s.copyFenced || !s.lastConsumer || !s.list || !s.allocator ||
       !SameCom(s.queue.Get(),s.submittedQueue.Get()) || !s.fences->input12 || !s.fences->output12 || !s.fences->retire11 ||
       !s.shared[1] || !s.imported[1] || s.depthWaitAttempted)return {};
    s.depthWaitAttempted=true;
    for(auto& event:s.depthReadyEvents){event=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!event)return {};}
    // retire11 covers the actual D3D11 copyback. output12 independently covers
    // the last consumer, including one submitted after Copyback was ordered.
    // Both were successfully signalled and flushed before this method; no new
    // frame, submission, queue wait, or user/game action is needed for progress.
    if(FAILED(GenshinHookCpuTiming::Call(GenshinHookCpuTiming::Stage::FenceArmApi,[&]{return s.fences->output12->SetEventOnCompletion(s.lastConsumer,s.depthReadyEvents[0]);},GenshinHookCpuTiming::ResultKind::Hresult)) ||
       FAILED(GenshinHookCpuTiming::Call(GenshinHookCpuTiming::Stage::FenceArmApi,[&]{return s.fences->retire11->SetEventOnCompletion(s.values->returned,s.depthReadyEvents[1]);},GenshinHookCpuTiming::ResultKind::Hresult)))return {};
    GenshinCpuStageTiming::Scope cpu(GenshinCpuStageTiming::Stage::BridgeDepthWait);
    const auto waited=GenshinHookCpuTiming::Call(GenshinHookCpuTiming::Stage::DepthCpuEventWait,[&]{return WaitForMultipleObjects(static_cast<DWORD>(s.depthReadyEvents.size()),s.depthReadyEvents.data(),TRUE,DepthCompletionWaitMs);},GenshinHookCpuTiming::ResultKind::Event);
    if(waited!=WAIT_OBJECT_0 && waited!=WAIT_TIMEOUT)return {};
    // An event wake (or timeout) is never evidence. Recheck real input,
    // queue wait, last-consumer and copyback fences, owner and alive devices.
    return Completed(owner);
}
Scope::Scope(const Owner& o,const std::array<ID3D11Resource*,4>& r,const void* p):owner(o),resources(r),parameters(p){
    previous_=active;
    if(active){if(active->lease)active->lease->Revoke();owner={};active=this;owns_=true;return;}
    if(!Valid(o) || !p)owner={};active=this;owns_=true;
}
Scope::~Scope(){if(owns_ && active==this)active=previous_;}
Scope* Scope::Current() noexcept{return active;}
}
