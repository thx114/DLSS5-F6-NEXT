#pragma once
#include <Windows.h>
#include <array>
#include <cstdint>

// CPU observations only. Fixed thread-local sampler state; no graphics queries or synchronization.
// Existing LOG_INFO transport may allocate/block/drop; these counters cover this sampler only.
#ifndef CALLER_CPU_TRACE_LOG
#define CALLER_CPU_TRACE_LOG(...) LOG_INFO(__VA_ARGS__)
#endif

namespace CallerCpuTrace
{
enum class Op : unsigned
{
    Evaluate, EvaluateInterval, EvaluateOutside, Init, CreateObjects, PrepareResources,
    AllocatorWait, AllocatorReset, ListReset, ProviderInit, ProviderEvaluate, NrBefore, NrAfter,
    Execute, Signal, CopyBack, InitWait,
    Present, PresentInterval, PresentOutside, TestPresent, HiddenPresent, FgPresent,
    InteropInit, NativeRingInit, SharedBuffer, SharedGetBuffer, SharedCreateTexture, SharedCreateHandle,
    SharedOpenHandle, Dx11Copy, Dx11CopyGetBuffer, Dx11Sync, Dx11Signal, Dx11Flush, CopyQueueWaitSubmit,
    CopyAllocatorWait, InteropCopy, CopyAllocatorReset, CopyListReset, FgGetBuffer, CopyExecute, CopySignal,
    NativeCopy, NativeProducer, NativeAllocatorReset, NativeListReset, NativeGetBuffer, NativeExecute, NativeSignal,
    PresentQueueWaitSubmit, CopyIdleWait, QueueIdle, QueueIdleWait, NrPresent, Resize, ResizeMutexWait, Count
};
inline constexpr const char* Names[] = {
    "srEvaluate", "srEntryInterval", "srOutsideUnknown", "srInit", "srCreateObjects", "srPrepareResources",
    "srAllocatorCpuWait", "srAllocatorReset", "srListReset", "srProviderInit", "srProviderEvaluate", "srNrBefore", "srNrAfter",
    "srExecuteSubmit", "srSignalSubmit", "srCopyBack", "srInitCpuWait",
    "interopPresent", "presentEntryInterval", "presentOutsideUnknown", "testRealPresent", "hiddenRealPresent", "fgPresent",
    "interopInit", "nativeRingInit", "requestSharedBuffer", "sharedGetBuffer", "sharedCreateTexture", "sharedCreateHandle",
    "sharedOpenHandle", "dx11CopyOrResolve", "dx11CopyGetBuffer", "dx11Sync", "dx11SignalSubmit", "dx11Flush", "copyQueueWaitSubmit",
    "copyAllocatorCpuWait", "interopCopyInclusive", "copyAllocatorReset", "copyListReset", "fgGetBuffer", "copyExecuteSubmit", "copySignalSubmit",
    "nativeCopyInclusive", "nativeProducer", "nativeAllocatorReset", "nativeListReset", "nativeFgGetBuffer", "nativeExecuteSubmit", "nativeSignalSubmit",
    "presentQueueWaitSubmit", "copyIdleCpuWait", "queueIdleInclusive", "queueIdleCpuWait", "nrPresentInclusive", "resizeInclusive", "resizeMutexCpuWait"
};
static_assert(sizeof(Names) / sizeof(Names[0]) == static_cast<unsigned>(Op::Count));
inline uint64_t Tick() { LARGE_INTEGER v{}; QueryPerformanceCounter(&v); return v.QuadPart; }
struct Stamp
{
    uint64_t owner = 0, handle = 0, frame = 0, slot = UINT64_MAX, required = 0, completed = 0;
    bool completedKnown = false;
};
struct Counts
{
    uint64_t calls = 0, ge20 = 0, ge50 = 0, emitted = 0, dropped = 0, maxTicks = 0;
    uint64_t maxStart = 0, maxEnd = 0;
    Stamp max{};
    unsigned highBudget = 0, lowBudget = 0;
};
struct Boundary
{
    uint64_t owner = 0, handle = 0, frame = 0, entry = 0, exit = 0;
    Op op = Op::Count;
};
struct ThreadState
{
    uint64_t hz = 0, window = 0, evictions = 0, missingBoundary = 0;
    DWORD pid = GetCurrentProcessId(), tid = GetCurrentThreadId();
    unsigned highBudget = 0, lowBudget = 0, nextBoundary = 0;
    std::array<Counts, static_cast<unsigned>(Op::Count)> counts{};
    std::array<Boundary, 8> boundaries{};
    ThreadState() { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); hz = f.QuadPart; }
};
inline thread_local ThreadState threadState;
inline double Ms(uint64_t ticks, uint64_t hz) { return hz ? double(ticks) * 1000.0 / double(hz) : 0.0; }

// Units: UTC FILETIME is 100 ns since 1601-01-01 UTC. The QPC bracket records anchor uncertainty.
inline void Anchor(uint64_t& utc, uint64_t& before, uint64_t& after)
{
    FILETIME ft{}; before = Tick(); GetSystemTimePreciseAsFileTime(&ft); after = Tick();
    utc = (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}
inline void RollWindow(uint64_t now)
{
    auto& s = threadState;
    if (s.window && now - s.window < s.hz * 5) return;
    uint64_t utc, before, after; Anchor(utc, before, after);
    if (!s.window)
        CALLER_CPU_TRACE_LOG("CallerCPU schema=1 marker=STAR-STALLS-CPU-7 pid={} tid={} hz={} utcFiletime100ns={} anchorQpcBefore={} anchorQpcAfter={} coverageComplete=false cpuOnly=true nested=true boundaryContinuity=thread_local budgetsPer5s=32ge50_8ge20 perOp=4ge50_1ge20 tailCountersMayBeUnflushed=true", s.pid, s.tid, s.hz, utc, before, after);
    else
    {
        for (unsigned i = 0; i < s.counts.size(); ++i)
        {
            const auto& c = s.counts[i];
            if (c.ge20)
                CALLER_CPU_TRACE_LOG("CallerCPU stats op={} pid={} tid={} hz={} qpc={} utcFiletime100ns={} anchorQpcBefore={} anchorQpcAfter={} calls={} ge20={} ge50={} emitted={} sampleDrops={} maxMs={:.3f} maxStart={} maxEnd={} maxOwner=0x{:X} maxHandle={} maxFrame={} maxSlot={} boundaryEvictions={} boundaryMissing={} counters=cumulative_thread_operation", Names[i], s.pid, s.tid, s.hz, now, utc, before, after, c.calls, c.ge20, c.ge50, c.emitted, c.dropped, Ms(c.maxTicks,s.hz), c.maxStart,c.maxEnd,c.max.owner,c.max.handle,c.max.frame,c.max.slot,s.evictions,s.missingBoundary);
        }
    }
    s.window = now; s.highBudget = s.lowBudget = 0;
    for (auto& c : s.counts) c.highBudget = c.lowBudget = 0;
}
inline void Observe(Op op, const Stamp& stamp, uint64_t start, uint64_t end,
                    uint64_t result = 0, bool resultKnown = false, uint64_t previousFrame = UINT64_MAX)
{
    auto& s = threadState;
    if (!s.hz || end < start) return;
    RollWindow(end);
    auto& c = s.counts[static_cast<unsigned>(op)];
    ++c.calls;
    const auto elapsed = end - start;
    if (elapsed > c.maxTicks) { c.maxTicks=elapsed; c.maxStart=start; c.maxEnd=end; c.max=stamp; }
    const double ms = Ms(elapsed,s.hz);
    if (ms < 20.0) return;
    ++c.ge20; const bool high = ms >= 50.0; if (high) ++c.ge50;
    auto& used = high ? c.highBudget : c.lowBudget;
    auto& totalUsed = high ? s.highBudget : s.lowBudget;
    if (used >= (high ? 4u : 1u) || totalUsed >= (high ? 32u : 8u)) { ++c.dropped; return; }
    ++used; ++totalUsed; ++c.emitted;
    uint64_t utc, before, after; Anchor(utc,before,after);
    CALLER_CPU_TRACE_LOG("CallerCPU span op={} pid={} tid={} owner=0x{:X} handle={} frame={} previousFrame={} slot={} qpcStart={} qpcEnd={} hz={} cpuMs={:.3f} ge50={} requiredFence={} completedBefore={} completedBeforeKnown={} resultKnown={} result={} utcFiletime100ns={} anchorQpcBefore={} anchorQpcAfter={} sampleDrops={} boundaryEvictions={} boundaryMissing={}", Names[static_cast<unsigned>(op)],s.pid,s.tid,stamp.owner,stamp.handle,stamp.frame,previousFrame,stamp.slot,start,end,s.hz,ms,high,stamp.required,stamp.completed,stamp.completedKnown,resultKnown,result,utc,before,after,c.dropped,s.evictions,s.missingBoundary);
}
class Scope
{
    Op _op; Stamp _stamp; uint64_t _start, _end = 0, _result = 0; bool _done = false, _resultKnown = false; Boundary* _boundary = nullptr;
public:
    Scope(Op op, const void* owner, uint64_t handle, uint64_t frame, uint64_t slot = UINT64_MAX,
          uint64_t required = 0, uint64_t completed = 0, bool boundary = false, bool completedKnown = false)
        : _op(op), _stamp{reinterpret_cast<uint64_t>(owner),handle,frame,slot,required,completed,completedKnown}, _start(Tick())
    {
        if (!boundary) return;
        auto& s=threadState;
        for (auto& b:s.boundaries)
            if (b.op==op && b.owner==_stamp.owner && b.handle==handle) { _boundary=&b; break; }
        if (_boundary && frame >= _boundary->frame && _boundary->exit && _boundary->exit <= _start)
        {
            Observe(op==Op::Present?Op::PresentInterval:Op::EvaluateInterval,_stamp,_boundary->entry,_start,0,false,_boundary->frame);
            Observe(op==Op::Present?Op::PresentOutside:Op::EvaluateOutside,_stamp,_boundary->exit,_start,0,false,_boundary->frame);
        }
        else
        {
            ++s.missingBoundary;
            if (!_boundary)
            {
                _boundary=&s.boundaries[s.nextBoundary++ % s.boundaries.size()];
                if (_boundary->op!=Op::Count) ++s.evictions;
            }
        }
        *_boundary={_stamp.owner,handle,frame,_start,0,op};
    }
    Scope(const Scope&)=delete; Scope& operator=(const Scope&)=delete;
    void Result(uint64_t result) { _result=result; _resultKnown=true; }
    void Stop() { if (!_end) _end=Tick(); }
    // R897: reuse already observed CPU endpoints; no clock/resource query.
    uint64_t BeginQpc() const noexcept { return _start; }
    uint64_t EndQpc() const noexcept { return _end; }
    void Finish(uint64_t result = 0, bool resultKnown = false)
    {
        if (_done) return;
        _done=true; Stop(); const auto end=_end;
        // Recursive re-entry/eviction must not attach another invocation's exit to this entry.
        if (_boundary && _boundary->entry==_start && _boundary->owner==_stamp.owner && _boundary->op==_op)
            _boundary->exit=end;
        Observe(_op,_stamp,_start,end,resultKnown?result:_result,resultKnown||_resultKnown);
    }
    ~Scope() { Finish(); }
};
}
