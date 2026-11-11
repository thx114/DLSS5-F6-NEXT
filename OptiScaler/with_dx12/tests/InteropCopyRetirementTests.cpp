#define NOMINMAX
#include <Windows.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

// Observable CPU operands only. The generated includes contain the actual
// production class, wait overloads and writer/Reset/submission blocks.
struct RefCounted
{
    ULONG references = 1;
    ULONG AddRef() { return ++references; }
    ULONG Release() { return --references; }
};
struct ID3D12Fence;
struct Event
{
    ID3D12Fence* fence = nullptr;
    UINT64 completedAfterWait = 0;
    DWORD result = WAIT_OBJECT_0;
    unsigned waits = 0;
    std::function<void()> duringWait;
};
struct ID3D12Fence : RefCounted
{
    UINT64 completed = 0, armed = 0;
    unsigned polls = 0, arms = 0;
    HRESULT armResult = S_OK;
    UINT64 GetCompletedValue() { ++polls; return completed; }
    HRESULT SetEventOnCompletion(UINT64 value, HANDLE event)
    {
        ++arms; armed = value;
        static_cast<Event*>(event)->fence = this;
        return armResult;
    }
};
DWORD FixtureWaitForSingleObject(HANDLE event, DWORD timeout)
{
    if (timeout != 5000) std::abort();
    auto& call = *static_cast<Event*>(event);
    ++call.waits;
    if (call.duringWait) call.duringWait();
    call.fence->completed = call.completedAfterWait;
    return call.result;
}
#define WaitForSingleObject FixtureWaitForSingleObject
#define LOG_ERROR(...) ((void)0)

struct ID3D12CommandAllocator : RefCounted
{
    unsigned resets = 0;
    HRESULT Reset() { ++resets; return S_OK; }
};
struct ID3D12CommandList : RefCounted {};
struct ID3D12Resource : RefCounted {};
struct ID3D12Device : RefCounted {};
struct IDXGISwapChain : RefCounted {};
struct ID3D12CommandQueue : RefCounted
{
    HRESULT signalResult = S_OK;
    unsigned executes = 0, signals = 0;
    ID3D12CommandList* executed = nullptr;
    ID3D12Fence* signalled = nullptr;
    UINT64 value = 0;
    void ExecuteCommandLists(UINT count, ID3D12CommandList* const* lists)
    {
        if (count != 1) std::abort();
        ++executes; executed = lists[0];
    }
    HRESULT Signal(ID3D12Fence* fence, UINT64 required)
    {
        ++signals; signalled = fence; value = required; return signalResult;
    }
};
struct State
{
    UINT64 frameCount = 31;
    static State& Instance() { static State value; return value; }
};
namespace CallerCpuTrace
{
enum class Op { CopyAllocatorWait, CopyAllocatorReset, CopyExecute, CopySignal };
struct Scope
{
    template<class... Args> explicit Scope(Args&&...) {}
    template<class... Args> void Finish(Args&&...) {}
    UINT64 BeginQpc() const { return 0; }
    UINT64 EndQpc() const { return 0; }
};
}
namespace GenshinBridgeFg
{
struct PresentScope
{
    static inline bool active = false;
    static bool Current() { return active; }
};
}
namespace GenshinHookCpuTiming
{
enum class StarRailOrigin { CopyRetirement };
enum class StarRailCopyRing { SharedColor, NativeOverwrite, Unknown };
enum class StarRailCall { CopyWait };
struct StarRailStamp
{
    StarRailCopyRing ring = StarRailCopyRing::Unknown;
    uintptr_t queue = 0, fence = 0;
    UINT slot = 0;
    UINT64 required = 0, completed = 0;
    DWORD result = 0;
    bool completedKnown = false, resultKnown = false;
};
struct WallSample { bool known; int64_t begin, end; };
bool StarRailTimingActive() { return false; }
StarRailStamp MakeStarRailStamp(StarRailOrigin, IDXGISwapChain*) { return {}; }
void RecordStarRailCall(StarRailCall, const StarRailStamp&, WallSample, bool) {}
}
std::string lastFailure;
bool RecordPrivateCopyFailure(const void*, const char* reason, HRESULT = S_OK,
                              std::optional<UINT64> = std::nullopt)
{
    lastFailure = reason;
    return false;
}
namespace HoYoShade
{
struct Isolation
{
    unsigned calls = 0;
    IDXGISwapChain* chain = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12Resource* color = nullptr;
    UINT64 generation = 0;
} isolation;
void QuarantineColor(IDXGISwapChain* chain, ID3D12CommandQueue* queue,
                     ID3D12Device* device, ID3D12Resource* color, UINT64 generation)
{
    ++isolation.calls;
    isolation.chain = chain; isolation.queue = queue;
    isolation.device = device; isolation.color = color; isolation.generation = generation;
    // Deliberately do not pin anything here: the production failure block must
    // keep real operands even if an isolation registry cannot allocate a slot.
}
}
struct SnapshotLease
{
    unsigned receipts = 0;
    void SubmittedRead12(ID3D12Fence*, UINT64) { ++receipts; }
};
#include "InteropSlotAdmissionProduction.h"

class Dx11wDx12SC : public RefCounted
{
  public:
    struct Receipt { const char* reason = nullptr; } _visibleReceipt;
    bool _sourceFrameLatencyOwner = false, _nativeParentRoute = false;
    std::atomic<bool> _sourceCopyUncertain { false };
    ID3D12Fence* _copyFence = nullptr;
    HANDLE _copyFenceEvent = nullptr;
    std::vector<ID3D12CommandAllocator*> _copyAllocators;
    std::vector<UINT64> _copyAllocatorFenceValues, _nrCopyAllocatorFenceValues;
    ID3D12CommandQueue* _dx12CommandQueue = nullptr;
    ID3D12Device* _dx12Device = nullptr;
    IDXGISwapChain* _fgSwapChain = nullptr;
    void* _handle = nullptr;
    UINT64 _copyFenceValue = 18, _lastInteropCopyFenceValue = 18;
    unsigned writes = 0;
    unsigned faultReports = 0;
    // Logging is observational: the exact extracted production wait below
    // still decides completion/Reset. No simulated device loss grants reuse.
    void _ReportCopyFenceFault(const char*,UINT,UINT64,UINT64,DWORD) { ++faultReports; }
    bool _WaitForCopyAllocator(UINT slot, std::vector<UINT64>& fenceValues);
    bool _WaitForCopyAllocator(UINT slot, std::vector<UINT64>& fenceValues, CopySlotAdmission& admission);

    bool WriteShared(UINT copySlot, CopySlotAdmission& admission)
    {
        admission.Clear();
#include "InteropWriterAdmissionProduction.inl"
        ++writes; // CPU observer at the real Resolve/Copy boundary, after production admission.
        return true;
    }
    bool ResetForCopy(UINT copySlot, CopySlotAdmission& admission)
    {
        auto* allocator = _copyAllocators[copySlot];
#include "InteropCopyResetProduction.inl"
        return true;
    }
    bool SubmitCopy(UINT copySlot, ID3D12CommandQueue* colorQueue,
                    Microsoft::WRL::ComPtr<ID3D12Resource> fgBackBuffer,
                    ID3D12CommandList* list, SnapshotLease* snapshotLease = nullptr, ID3D12Resource** copiedColour = nullptr)
    {
        if(copiedColour)*copiedColour=nullptr;
        ID3D12CommandList* lists[] = { list };
        CallerCpuTrace::Scope copyExecuteCpu(CallerCpuTrace::Op::CopyExecute);
        HRESULT result = E_PENDING;
#include "InteropCopySubmissionProduction.inl"
        return true;
    }
};
#include "InteropCopyWaitProduction.inl"

namespace
{
unsigned checks = 0;
void Check(bool ok, const char* message)
{
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
struct Fixture
{
    ID3D12Fence fence;
    ID3D12CommandAllocator allocator;
    ID3D12CommandList list;
    ID3D12CommandQueue queue, otherQueue;
    ID3D12Device device;
    ID3D12Resource color;
    IDXGISwapChain chain;
    Event event;
    Dx11wDx12SC owner;
    Fixture()
    {
        owner._copyFence = &fence; owner._copyFenceEvent = &event;
        owner._copyAllocators = { &allocator }; owner._copyAllocatorFenceValues = { 18 };
        owner._dx12CommandQueue = &queue; owner._dx12Device = &device; owner._fgSwapChain = &chain;
        fence.completed = 17; event.completedAfterWait = 18;
        GenshinBridgeFg::PresentScope::active = false;
        HoYoShade::isolation = {};
    }
};
void ReaderRetiresBeforeWriteOnce()
{
    for (bool starRail : { false, true })
    {
        Fixture f;
        f.owner._sourceFrameLatencyOwner = starRail;
        f.event.duringWait = [&] { Check(f.owner.writes == 0 && f.allocator.resets == 0,
                                       "pending reader is retired before writes and Reset"); };
        CopySlotAdmission admission;
        Check(f.owner.WriteShared(0, admission), "ordinary and StarRail writers acquire the exact retired slot");
        Check(f.event.waits == 1 && f.fence.armed == 18 && f.owner.writes == 1, "one existing wait admits one write");
        const auto polls = f.fence.polls;
        Check(f.owner.ResetForCopy(0, admission) && f.allocator.resets == 1, "Reset consumes the pre-write proof");
        Check(f.event.waits == 1 && f.fence.polls == polls, "Reset neither waits nor polls again");
        Check(!f.owner.ResetForCopy(0, admission) && f.allocator.resets == 1, "a consumed proof cannot admit another Reset");
        Check(f.owner._sourceFrameLatencyOwner == starRail && !f.owner._nativeParentRoute, "GPU admission grants no source-owner rights");
    }
    Fixture f;
    f.fence.completed = 18;
    CopySlotAdmission ready;
    Check(f.owner.WriteShared(0, ready) && f.owner.ResetForCopy(0, ready) && f.event.waits == 0,
          "an already completed reader adds no wait");
    f.owner._copyAllocatorFenceValues[0] = 0;
    CopySlotAdmission fresh;
    Check(f.owner.WriteShared(0, fresh) && f.owner.ResetForCopy(0, fresh) && f.event.waits == 0,
          "a never-submitted slot needs no wait");
}
void RefusalsNeverWriteOrReset()
{
    for (int kind = 0; kind != 7; ++kind)
    {
        Fixture f;
        if (kind == 0) { f.owner._nativeParentRoute = true; GenshinBridgeFg::PresentScope::active = true; }
        if (kind == 1) f.event.result = WAIT_TIMEOUT;
        if (kind == 2) f.event.completedAfterWait = 17; // Event acknowledgement is not completion.
        if (kind == 3) f.fence.completed = UINT64_MAX;
        if (kind == 4) f.event.completedAfterWait = UINT64_MAX;
        if (kind == 5) f.fence.armResult = E_FAIL;
        if (kind == 6) f.owner._copyFenceEvent = nullptr;
        CopySlotAdmission admission;
        Check(!f.owner.WriteShared(0, admission) && f.owner.writes == 0 && f.allocator.resets == 0,
              "Busy, timeout, missing objects, failed arm and unproved completion refuse before writing");
        Check(!f.owner.ResetForCopy(0, admission) && f.allocator.resets == 0,
              "failed admission cannot be repaired at allocator Reset");
        if(kind>=1&&kind<=4)Check(f.owner.faultReports==1,
            "timeout/invalid/false event completion records a fault without authorizing writes or Reset");
        if (kind == 0)
        {
            Check(f.event.waits == 0 && f.fence.arms == 0, "native-parent optional Busy adds no wait");
            Check(f.owner._visibleReceipt.reason &&
                  std::string(f.owner._visibleReceipt.reason) == "CopySlotBusy",
                  "early admission preserves the caller's nonterminal native Busy classification");
        }
    }
}
void ProofCannotCrossIdentityOrTimeline()
{
    Fixture f;
    CopySlotAdmission admission;
    Check(f.owner.WriteShared(0, admission), "acquire proof for mismatch test");
    f.owner._copyAllocatorFenceValues[0] = 19;
    Check(!f.owner.ResetForCopy(0, admission) && f.allocator.resets == 0 && f.event.waits == 1,
          "an intervening submission invalidates proof without a late wait");
    f.owner._copyAllocatorFenceValues[0] = 18;
    Check(!f.owner.ResetForCopy(0, admission), "restoring old numbers cannot revive a revoked proof");
    for (int kind = 0; kind != 6; ++kind)
    {
        int owner = 0, anotherOwner = 0, ring = 0, anotherRing = 0;
        ID3D12Fence fence, anotherFence;
        ID3D12CommandAllocator allocator, anotherAllocator;
        CopySlotAdmission proof;
        proof.Record(&owner, &ring, &fence, &allocator, 0, 18);
        Check(!proof.Consume(kind == 0 ? &anotherOwner : &owner, kind == 1 ? &anotherRing : &ring,
                             kind == 2 ? &anotherFence : &fence, kind == 3 ? &anotherAllocator : &allocator,
                             kind == 4 ? 1u : 0u, kind == 5 ? 19 : 18), "owner/ring/fence/allocator/slot/value must all match");
        Check(!proof.Consume(&owner, &ring, &fence, &allocator, 0, 18), "mismatched proof is consumed permanently");
    }
}
void FailedSignalPinsActualOperandsOnEveryRoute()
{
    for (int route = 0; route != 3; ++route)
    {
        Fixture f;
        f.owner._sourceFrameLatencyOwner = route == 1;
        f.owner._nativeParentRoute = route == 2;
        auto* actualQueue = route == 2 ? &f.otherQueue : &f.queue;
        actualQueue->signalResult = E_FAIL;
        SnapshotLease lease;
        Check(!f.owner.SubmitCopy(0, actualQueue, &f.color, &f.list, &lease), "actual Execute followed by Signal failure is refused");
        Check(actualQueue->executes == 1 && actualQueue->signals == 1 && actualQueue->executed == &f.list &&
              actualQueue->signalled == &f.fence && actualQueue->value == 19, "the failed receipt belongs to the actual writer queue");
        Check(f.owner._sourceCopyUncertain && f.owner._copyAllocatorFenceValues[0] == UINT64_MAX &&
              f.owner._lastInteropCopyFenceValue == 0 && lease.receipts == 0, "uncertainty poisons reuse and never publishes old/success receipts");
        Check(f.owner.references == 2 && actualQueue->references == 2 && f.device.references == 2 && f.color.references == 2,
              "real wrapper/rings, actual queue, device and acquired destination remain pinned without registry assistance");
        Check(HoYoShade::isolation.calls == 1 && HoYoShade::isolation.chain == &f.chain &&
              HoYoShade::isolation.queue == actualQueue && HoYoShade::isolation.device == &f.device &&
              HoYoShade::isolation.color == &f.color, "quarantine names the exact submitted operands");
        CopySlotAdmission admission;
        const auto waits = f.event.waits;
        Check(!f.owner.WriteShared(0, admission) && !f.owner.ResetForCopy(0, admission) &&
              f.owner.writes == 0 && f.event.waits == waits && f.allocator.resets == 0,
              "ordinary/native/StarRail quarantine blocks later writes and Reset before waiting");
        Check(f.owner._sourceFrameLatencyOwner == (route == 1) && f.owner._nativeParentRoute == (route == 2),
              "quarantine never expands special owner privileges");
        Check(f.owner._copyAllocators.size() == 1 && f.owner._copyAllocatorFenceValues.size() == 1,
              "retention does not expand rings");
    }
    Fixture f;
    SnapshotLease lease;
    Check(f.owner.SubmitCopy(0, &f.queue, &f.color, &f.list, &lease), "successful Signal publishes this actual submission");
    Check(!f.owner._sourceCopyUncertain && f.owner._lastInteropCopyFenceValue == 19 &&
          f.owner._copyAllocatorFenceValues[0] == 19 && lease.receipts == 1, "success retains exact signal provenance");
    Check(f.owner.references == 1 && f.queue.references == 1 && f.device.references == 1 &&
          f.color.references == 1 && HoYoShade::isolation.calls == 0, "ordinary success adds no permanent pins");
    ID3D12Resource* copied=nullptr;
    Check(f.owner.SubmitCopy(0, &f.queue, &f.color, &f.list, nullptr, &copied), "optional colour witness follows actual successful Signal");
    Check(copied==&f.color && f.owner._lastInteropCopyFenceValue==20, "returned destination names this exact copied frame");
    copied->Release();
    f.queue.signalResult=E_FAIL;copied=&f.color;
    Check(!f.owner.SubmitCopy(0, &f.queue, &f.color, &f.list, nullptr, &copied) && !copied, "failed Signal never returns an optional colour witness");
}
}
int main()
{
    static_assert(!std::is_copy_constructible_v<CopySlotAdmission>);
    static_assert(!std::is_move_constructible_v<CopySlotAdmission>);
    ReaderRetiresBeforeWriteOnce();
    RefusalsNeverWriteOrReset();
    ProofCannotCrossIdentityOrTimeline();
    FailedSignalPinsActualOperandsOnEveryRoute();
    std::printf("PASS %u common interop production CPU checks; GPU/SDK/game untested\n", checks);
}
