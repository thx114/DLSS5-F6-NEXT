// Real production owners and methods are included below; doubles implement only
// COM, GPU state destruction and passive observation/colour publication seams.
#include <framegen/dlssg/NativeGuideListInterest.h>
#include <framegen/dlssg/DlssgInputContinuity.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <vector>

using UINT = unsigned int;
using UINT64 = uint64_t;
#define BUFFER_COUNT 4
constexpr int D3D12_COMMAND_LIST_TYPE_DIRECT = 0;
inline bool FAILED(int value) { return value < 0; }
inline unsigned GetCurrentProcessId() { return 1; }
template<class... Args> void LogIgnored(const char*, const Args&...) {}
#define LOG_INFO(...) LogIgnored(__VA_ARGS__)
struct RefCount
{
    unsigned references = 0, releases = 0;
    void AddRef() { ++references; }
    void Release() { --references; ++releases; }
};
struct ID3D12CommandList : RefCount {};
struct ID3D12Device {};
struct ID3D12Resource : RefCount {};
struct ID3D12Fence : RefCount
{
    UINT64 value = 0;
    UINT64 GetCompletedValue() { return value; }
};
struct ID3D12CommandQueue : RefCount
{
    struct SignalReceipt { ID3D12Fence* fence; UINT64 value; };
    std::vector<SignalReceipt> signals;
    int result = 0, type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12Device* device = nullptr;
    std::function<void()> onSignal;
    struct Desc { int Type; };
    Desc GetDesc() { return {type}; }
    int Signal(ID3D12Fence* fence, UINT64 value)
    {
        signals.push_back({fence, value});
        if (onSignal) { auto callback = std::move(onSignal); callback(); }
        return result;
    }
};
namespace dlssg::depth_contract
{
inline bool OnDevice(ID3D12Device* device, ID3D12CommandQueue* queue) { return queue && device == queue->device; }
}
namespace Microsoft::WRL
{
template<class T> struct ComPtr
{
    T* ptr = nullptr;
    ComPtr() = default;
    ComPtr(const ComPtr& other) { *this = other.ptr; }
    ComPtr(ComPtr&& other) noexcept : ptr(other.ptr) { other.ptr = nullptr; }
    ~ComPtr() { Reset(); }
    T* Get() const { return ptr; }
    T* operator->() const { return ptr; }
    explicit operator bool() const { return ptr != nullptr; }
    void Reset() { if (ptr) ptr->Release(); ptr = nullptr; }
    ComPtr& operator=(T* value)
    {
        if (value) value->AddRef();
        Reset(); ptr = value; return *this;
    }
    ComPtr& operator=(const ComPtr& other) { return *this = other.ptr; }
    ComPtr& operator=(ComPtr&& other) noexcept
    {
        if (this != &other) { Reset(); ptr = other.ptr; other.ptr = nullptr; }
        return *this;
    }
};
}
struct DS_Dx12
{
    inline static int live = 0;
    DS_Dx12() { ++live; }
    ~DS_Dx12() { --live; }
};
struct RF_Dx12
{
    inline static int live = 0;
    RF_Dx12() { ++live; }
    ~RF_Dx12() { --live; }
};
struct FG_Constants {};
namespace sl { struct Constants {}; }
namespace dlssg::zzz { struct Receipt {}; }
namespace GuideDiagnostics
{
struct Snapshot
{
    struct Rect { UINT x = 0, y = 0, width = 0, height = 0; };
    struct Colour { bool accepted = false; UINT format = 0; UINT64 width = 0; UINT height = 0; Rect rect; } colour;
    UINT64 epoch = 1;
    const char* colourChoice = nullptr;
};
}
enum class FG_ResourceType { Depth, Velocity, HudlessColor };
struct ColourSource
{
    bool bridge = false;
    struct Key { UINT64 frame = 0; uintptr_t output = 0; } key;
    uintptr_t list = 0;
    struct Description { UINT Format = 7; UINT64 Width = 1920; UINT Height = 1080; } outputDescription;
};
struct DLSSG_Dx12
{
#define NATIVE_GUIDE_TYPES_ONLY
#include "NativeGuideNotificationsProduction.inl"
#undef NATIVE_GUIDE_TYPES_ONLY
    std::recursive_mutex _inputContinuityMutex;
    std::shared_mutex _resourceMutex[BUFFER_COUNT];
    std::map<FG_ResourceType, int> _frameResources[BUFFER_COUNT], _resourceReady[BUFFER_COUNT];
    struct GuideTag { bool accepted = false; UINT64 frame = 0; } _guideTags[BUFFER_COUNT][2];
    struct Observation
    {
        GuideDiagnostics::Snapshot snapshot;
        UINT64 frame = 0, value = 0;
        ID3D12Fence* fence = nullptr;
    } _guideObservations[BUFFER_COUNT];
    std::shared_ptr<ColourSource> _upscalerColourSources[BUFFER_COUNT];
    bool _upscalerColourSucceeded[BUFFER_COUNT] {};
    dlssg::InputContinuity<BUFFER_COUNT> _inputContinuity;
    dlssg::NativeGuideListInterest _nativeGuideListInterest;
    std::vector<std::shared_ptr<NativeGuideUse>> _nativeGuideRecordings;
    NativeGuideUse* _nativeGuideFrames[BUFFER_COUNT] {};
    bool _teardownQuarantined = false, _nativeGuideFailed = false, _managedRetained = false;
    UINT64 _nativeGuideSubmissions = 0, _nativeGuideSerial = 0, _nativeGuideExecution = 0;
    UINT64 _frameCount = 1, _inputOwnerEpoch = 1;
    unsigned deactivations = 0, observations = 0, refusals = 0;
    ID3D12Device* _device = nullptr;
    UINT GetIndex() const { return static_cast<UINT>(_frameCount % BUFFER_COUNT); }
    bool TracksNativeGuides() const { return true; }
    void Deactivate() { ++deactivations; }
    void ObserveGuideSubmission(UINT slot, ID3D12Fence* fence, UINT64 value)
    {
        ++observations;
        _guideObservations[slot].fence = fence; _guideObservations[slot].value = value;
    }
    bool UpscalerColourCurrent(const std::shared_ptr<ColourSource>& source) const { return bool(source); }
    void RefuseDepthContract(const char*, bool) { ++refusals; }
    bool NativeGuideRetired(const NativeGuideUse&) const;
    std::shared_ptr<NativeGuideUse> AcquireNativeGuideRecording();
    void ClearNativeGuideFrame(UINT);
    dlssg::NativeSource NativeGuideFrameSource(UINT) const;
    bool NativeGuidesRetired() const;
    UINT64 NativeGuidesSubmitting(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
    void NativeGuidesSubmitted(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*, UINT64);
    void NativeGuidesReset(ID3D12CommandList*);
    std::shared_ptr<NativeGuideUse> FindNativeGuideSource(const dlssg::NativeSource&);
    void CompleteNativeGuideSource(ID3D12CommandList*, const dlssg::NativeSource&, ID3D12Resource*, bool);
};
#include "NativeGuideNotificationsProduction.inl"

namespace
{
unsigned checks = 0;
struct Failure {};
void Check(bool okay, const char* text)
{
    ++checks;
    if (!okay) { std::printf("FAIL %s\n", text); throw Failure{}; }
}
// Test setup mirrors only publication, never reservation or retirement logic.
DLSSG_Dx12::NativeGuideUse* Publish(DLSSG_Dx12& p, UINT64 frame,
    ID3D12CommandList& wrapper, ID3D12CommandList& native, ID3D12Fence& fence,
    ID3D12Resource* output = nullptr)
{
    p._frameCount = frame;
    auto lease = p.AcquireNativeGuideRecording();
    auto& use = *lease;
    use.source = {};
    use.source.serial = ++p._nativeGuideSerial; use.source.frame = frame;
    use.source.epoch = p._inputOwnerEpoch; use.source.generation = p._inputContinuity.SourceGeneration();
    use.source.list = reinterpret_cast<uintptr_t>(&wrapper); use.source.output = reinterpret_cast<uintptr_t>(output);
    use.list = &wrapper; use.nativeList = &native; use.fence = &fence; use.output = output;
    use.queue.Reset(); ++use.value; use.execution = 0;
    use.closed = use.submitted = use.executing = use.signalled = use.srSucceeded = use.ordered = false;
    use.dispatched = use.presented = false;
    const auto slot = p.GetIndex();
    p.ClearNativeGuideFrame(slot);
    p._nativeGuideFrames[slot] = &use;
    p._nativeGuideListInterest.Publish(&use, &wrapper, &native);
    for (auto type : {FG_ResourceType::Depth, FG_ResourceType::Velocity, FG_ResourceType::HudlessColor})
        p._frameResources[slot][type] = p._resourceReady[slot][type] = 1;
    for (auto& tag : p._guideTags[slot]) { tag.accepted = true; tag.frame = frame; }
    p._guideObservations[slot] = {}; p._guideObservations[slot].frame = frame;
    return &use;
}
UINT64 Reserve(DLSSG_Dx12& p, ID3D12CommandQueue& queue, ID3D12CommandList& list)
{
    ID3D12CommandList* batch[] {&list};
    return p.NativeGuidesSubmitting(&queue, 1, batch);
}
void Submitted(DLSSG_Dx12& p, ID3D12CommandQueue& queue, ID3D12CommandList& list, UINT64 execution)
{
    ID3D12CommandList* batch[] {&list};
    p.NativeGuidesSubmitted(&queue, 1, batch, execution);
}
template<class Action> bool RunsWhileContinuityHeld(DLSSG_Dx12& p, Action action)
{
    std::promise<void> entered;
    auto enteredFuture = entered.get_future();
    std::unique_lock held(p._inputContinuityMutex);
    auto worker = std::async(std::launch::async, [&] { entered.set_value(); action(); });
    enteredFuture.wait();
    const bool completed = worker.wait_for(std::chrono::milliseconds(150)) == std::future_status::ready;
    held.unlock(); worker.get(); return completed;
}
void ShortLockAndReceipts(bool asynchronous)
{
    ID3D12CommandList wrapper, native, unrelated;
    ID3D12Fence fence;
    ID3D12CommandQueue queue;
    DLSSG_Dx12 p;
    auto* use = Publish(p, 1, wrapper, native, fence);
    Check(RunsWhileContinuityHeld(p, [&] { p.NativeGuidesReset(&unrelated); }), "unrelated Reset bypasses continuity");
    Check(RunsWhileContinuityHeld(p, [&] { Reserve(p, queue, unrelated); }), "unrelated pre-Execute bypasses continuity");
    Check(RunsWhileContinuityHeld(p, [&] { Submitted(p, queue, unrelated, 1); }), "unrelated post-Execute bypasses continuity");
    Check(!use->closed && !use->submitted && queue.signals.empty(), "unrelated callbacks leave producer unchanged");
    Check(!RunsWhileContinuityHeld(p, [&] { p.NativeGuidesReset(&native); }), "native alias still takes continuity");
    Check(use->closed && p.NativeGuideRetired(*use), "unsubmitted successful Reset retires recording");
    use->closed = false;
    ID3D12CommandList* mixed[] {&unrelated, &wrapper};
    UINT64 execution = 0;
    Check(!RunsWhileContinuityHeld(p, [&] { execution = p.NativeGuidesSubmitting(&queue, 2, mixed); }), "mixed batch still takes continuity");
    Check(execution && use->execution == execution && use->submitted && use->executing && use->queue.Get() == &queue,
          "pre-Execute receipt binds exact queue and recording");
    p.NativeGuidesReset(&native);
    Check(!p.NativeGuideRetired(*use), "Reset racing Execute cannot mint completion");
    if (asynchronous)
    {
        auto worker = std::async(std::launch::async, [&] { p.NativeGuidesSubmitted(&queue, 2, mixed, execution); });
        worker.get();
    }
    else p.NativeGuidesSubmitted(&queue, 2, mixed, execution);
    Check(queue.signals.size() == 1 && use->signalled && !use->executing && p.observations == 1,
          "sync/async post-Execute signals exact reservation after Reset");
    Check(queue.signals[0].fence == &fence && queue.signals[0].value == use->value, "receipt targets producer fence/value");
    Check(!p.NativeGuideRetired(*use), "pending producer fence blocks retirement");
    fence.value = use->value;
    Check(p.NativeGuideRetired(*use) && p.NativeGuidesRetired(), "own producer completion plus Reset permits retirement");
    Submitted(p, queue, native, execution);
    Check(queue.signals.size() == 1, "duplicate post-Execute cannot signal again");
    fence.value = UINT64_MAX;
    Check(!p.NativeGuideRetired(*use), "device removed fence never retires");
}
void SixListsAndCompaction()
{
    std::array<ID3D12CommandList, 9> wrappers, natives;
    std::array<ID3D12Fence, 9> fences;
    std::array<std::array<ID3D12Resource, 2>, 6> copies;
    ID3D12CommandQueue queue;
    DLSSG_Dx12 p;
    std::array<DLSSG_Dx12::NativeGuideUse*, 6> owners {};
    for (UINT i = 0; i < 6; ++i)
    {
        auto* use = Publish(p, i + 1, wrappers[i], natives[i], fences[i]);
        owners[i] = use;
        use->depthScale = std::make_unique<DS_Dx12>();
        for (UINT k = 0; k < 2; ++k) { use->flip[k] = std::make_unique<RF_Dx12>(); use->copies[k] = &copies[i][k]; }
        const auto receipt = Reserve(p, queue, natives[i]);
        Submitted(p, queue, wrappers[i], receipt);
        Check(p._nativeGuideRecordings.size() == i + 1, "pending owners grow independently of four presentation slots");
    }
    Check(DS_Dx12::live == 6 && RF_Dx12::live == 12, "six recordings own independent DS/RF resources");
    for (UINT i = 0; i < 6; ++i)
        Check(owners[i]->copies[0].Get() == &copies[i][0] && copies[i][0].references == 1 &&
              p._nativeGuideListInterest.MayContain(&wrappers[i]) && p._nativeGuideListInterest.MayContain(&natives[i]),
              "all six owner resources and list aliases survive slot wrap");
    Check(p.NativeGuideFrameSource(1).frame == 5 && p.NativeGuideFrameSource(2).frame == 6 &&
          p.NativeGuideFrameSource(3).frame == 3 && p.NativeGuideFrameSource(0).frame == 4 &&
          !p.NativeGuideFrameSource(BUFFER_COUNT), "four slots expose only exact latest frame identities");
    fences[0].value = owners[0]->value;
    Check(!p.NativeGuideRetired(*owners[0]), "completed producer without successful Reset cannot recycle");
    for (UINT i = 0; i < 6; ++i) p.NativeGuidesReset(&natives[i]);
    for (UINT i = 0; i < 5; ++i) fences[i].value = owners[i]->value;
    Check(!p.NativeGuideRetired(*owners[5]), "other five fences cannot retire sixth owner");
    const auto sixthSource = owners[5]->source;
    Publish(p, 7, wrappers[6], natives[6], fences[6]);
    Check(p._nativeGuideRecordings.size() == 2 && DS_Dx12::live == 2 && RF_Dx12::live == 4,
          "sweep retains pending sixth owner, reuses one retired owner and deletes excess retired GPU owners");
    Check(p._nativeGuideFrames[2] == owners[5] && p.NativeGuideFrameSource(2) == sixthSource &&
          p._guideTags[2][0].accepted, "retiring other owners cannot clear pending owner's slot");
    Check(!p._nativeGuideFrames[0] && !p._nativeGuideFrames[1] && !p._guideTags[0][0].accepted &&
          !p._guideTags[1][1].accepted, "retirement clears tags on exact owner slots, not current frame slot");
    for (UINT slot : {0U, 1U})
        Check(!p._frameResources[slot].contains(FG_ResourceType::Depth) &&
              !p._resourceReady[slot].contains(FG_ResourceType::Velocity) &&
              p._frameResources[slot].contains(FG_ResourceType::HudlessColor), "frame clear removes only guide identity maps");
    for (UINT i = 0; i < 5; ++i)
        Check(!p._nativeGuideListInterest.MayContain(&wrappers[i]), "retired list interests erased by recording key");
    Check(p._nativeGuideListInterest.MayContain(&wrappers[5]), "pending old list interest survives four-slot wrap");
    unsigned retainedCopies = 0;
    for (const auto& pair : copies) for (const auto& resource : pair) retainedCopies += resource.references;
    Check(retainedCopies == 4, "only reused and pending owners retain their copies");
    fences[5].value = owners[5]->value;
    Publish(p, 8, wrappers[7], natives[7], fences[7]);
    Check(p._nativeGuideRecordings.size() == 2 && !p._nativeGuideListInterest.MayContain(&wrappers[5]),
          "sixth owner retires only when its own fence completes");
    p.NativeGuidesReset(&wrappers[6]); p.NativeGuidesReset(&wrappers[7]);
    Publish(p, 9, wrappers[8], natives[8], fences[8]);
    Check(p._nativeGuideRecordings.size() == 1 && DS_Dx12::live == 1 && RF_Dx12::live == 2,
          "fully retired peak owners fall back to one reusable GPU owner");
    p.ClearNativeGuideFrame(p.GetIndex());
    retainedCopies = 0;
    for (const auto& pair : copies) for (const auto& resource : pair) retainedCopies += resource.references;
    Check(p._nativeGuideRecordings.size() == 1 && DS_Dx12::live == 1 && retainedCopies == 2 &&
          p._nativeGuideListInterest.MayContain(&wrappers[8]), "clearing borrowed frame never retires recording resources or interest");
}
void SameListGenerations(bool asynchronous)
{
    ID3D12CommandList wrapper, native;
    ID3D12Fence oldFence, newFence;
    ID3D12CommandQueue queue;
    DLSSG_Dx12 p;
    auto* old = Publish(p, 1, wrapper, native, oldFence);
    const auto first = Reserve(p, queue, wrapper);
    p.NativeGuidesReset(&native);
    auto* fresh = Publish(p, 5, wrapper, native, newFence);
    Check(fresh != old && p._nativeGuideRecordings.size() == 2, "same-list Reset with old fence pending creates independent generation");
    const auto second = Reserve(p, queue, native);
    Check(second && second != first, "new generation gets distinct execution receipt");
    if (asynchronous)
    {
        auto worker = std::async(std::launch::async, [&] { Submitted(p, queue, native, first); });
        worker.get();
    }
    else Submitted(p, queue, wrapper, first);
    Check(queue.signals.size() == 1 && old->signalled && !old->executing && fresh->executing && !fresh->signalled,
          "old Execute callback cannot claim newer reservation of same list");
    Check(p.observations == 0 && !p._guideObservations[1].fence && p._guideObservations[1].frame == 5,
          "old completion cannot update new frame observation");
    Submitted(p, queue, wrapper, first);
    Submitted(p, queue, native, 0);
    Check(queue.signals.size() == 1 && fresh->executing, "duplicate and zero receipts cannot consume new reservation");
    Submitted(p, queue, wrapper, second);
    Check(queue.signals.size() == 2 && queue.signals[1].fence == &newFence && fresh->signalled &&
          p.observations == 1 && p._guideObservations[1].fence == &newFence,
          "new callback alone publishes new fence and observation");
    oldFence.value = old->value;
    Check(p.NativeGuideRetired(*old) && !p.NativeGuideRetired(*fresh), "old completion does not retire new generation");
}
void SelectedCompletion()
{
    ID3D12CommandList oldList, newList;
    ID3D12Resource oldOutput, newOutput;
    ID3D12Fence oldFence, newFence;
    DLSSG_Dx12 p;
    auto* old = Publish(p, 1, oldList, oldList, oldFence, &oldOutput);
    const auto oldSource = old->source;
    auto* fresh = Publish(p, 5, newList, newList, newFence, &newOutput);
    auto colour = std::make_shared<ColourSource>();
    colour->key.frame = 5; colour->key.output = fresh->source.output; colour->list = fresh->source.list;
    p._upscalerColourSources[1] = colour;
    p.CompleteNativeGuideSource(&oldList, oldSource, &oldOutput, true);
    Check(old->srSucceeded && !fresh->srSucceeded && !p._upscalerColourSucceeded[1] &&
          !p._guideObservations[1].snapshot.colour.accepted, "late old SR completion updates only its original recording");
    p.CompleteNativeGuideSource(&newList, fresh->source, &oldOutput, true);
    Check(!fresh->srSucceeded && p.refusals == 1, "changed output cannot mint selected SR success");
    p.CompleteNativeGuideSource(&newList, fresh->source, &newOutput, true);
    Check(fresh->srSucceeded && p._upscalerColourSucceeded[1] &&
          p._guideObservations[1].snapshot.colour.accepted, "exact selected SR completion updates matching frame observation");
    auto changed = fresh->source; ++changed.serial;
    Check(!p.FindNativeGuideSource(changed), "source lookup requires full immutable transaction identity");
}
void MixedBatchAndClosedReplay()
{
    ID3D12CommandList firstList, firstNative, secondList, secondNative, unrelated;
    ID3D12Fence firstFence, secondFence;
    ID3D12CommandQueue queue, otherQueue;
    DLSSG_Dx12 p;
    auto* first = Publish(p, 1, firstList, firstNative, firstFence);
    auto* second = Publish(p, 2, secondList, secondNative, secondFence);
    second->value = 7;
    ID3D12CommandList* batch[] {&unrelated, &firstNative, &secondList};
    const auto receipt = p.NativeGuidesSubmitting(&queue, 3, batch);
    p.NativeGuidesReset(&firstList); p.NativeGuidesReset(&secondNative);
    p.NativeGuidesSubmitted(&queue, 3, batch, receipt);
    Check(receipt && first->execution == receipt && second->execution == receipt &&
          first->signalled && second->signalled && queue.signals.size() == 2 && p.observations == 2,
          "mixed batch preserves both exact recording receipts under one Execute");
    firstFence.value = first->value; secondFence.value = 6;
    Check(p.NativeGuideRetired(*first) && !p.NativeGuideRetired(*second), "mixed batch keeps independent fence targets");
    secondFence.value = 7;
    Check(p.NativeGuidesRetired(), "batch drain requires both own producer fences");
    Reserve(p, otherQueue, secondNative);
    Check(!p._nativeGuideFailed && !otherQueue.references, "closed recording cannot be replayed as a live reservation");
    p._teardownQuarantined = true;
    first->closed = false;
    p.NativeGuidesReset(&firstList);
    Check(!first->closed && !Reserve(p, queue, firstList), "quarantined callbacks cannot close or reserve owners");
    p._teardownQuarantined = false;
    p._nativeGuideExecution = UINT64_MAX - 1;
    Check(!Reserve(p, queue, firstList) && p._nativeGuideFailed && p._managedRetained,
          "execution receipt saturation fails closed without wrapping generations");
}
void CpuLeaseAndSignalReentry()
{
    ID3D12CommandList firstList, secondList, thirdList;
    ID3D12Fence firstFence, secondFence, thirdFence;
    ID3D12CommandQueue queue;
    DLSSG_Dx12 p;
    auto* first = Publish(p, 1, firstList, firstList, firstFence);
    first->depthScale = std::make_unique<DS_Dx12>();
    auto lease = p.FindNativeGuideSource(first->source);
    p.NativeGuidesReset(&firstList);
    Check(p.NativeGuideRetired(*first), "CPU lease test still satisfies real GPU retirement");
    Check(!p.NativeGuidesRetired(), "provider drain cannot discard an owner leased by a CPU stack");
    auto* second = Publish(p, 2, secondList, secondList, secondFence);
    Check(second != first && lease.get() == first && first->source.frame == 1 && first->depthScale &&
          p._nativeGuideRecordings.size() == 2 && p._nativeGuideListInterest.MayContain(&firstList),
          "CPU stack lease prevents recursive acquisition recycling retired owner's GPU state");
    lease.reset();
    auto* third = Publish(p, 3, thirdList, thirdList, thirdFence);
    Check(third == first && third->depthScale && p._nativeGuideRecordings.size() == 2 &&
          !p._nativeGuideListInterest.MayContain(&firstList), "releasing CPU lease permits later real retired-owner reuse");
    const auto receipt = Reserve(p, queue, secondList);
    queue.onSignal = [&] {
        p.NativeGuidesReset(&secondList);
        // Recursive publications force registry reallocation inside real Signal.
        // Local arrays remain alive until the test provider is drained below.
        p._frameCount = 100;
        auto recursive = p.AcquireNativeGuideRecording();
        Check(recursive.get() != second, "Signal stack owner cannot be reclaimed by recursive acquisition");
        for (UINT i = 0; i < 32; ++i)
        {
            p._frameCount = 101 + i;
            auto extra = p.AcquireNativeGuideRecording();
            extra->list = &thirdList; extra->nativeList = &thirdList;
            extra->source.frame = p._frameCount; extra->source.epoch = p._inputOwnerEpoch;
            p._nativeGuideListInterest.Publish(extra.get(), &thirdList, &thirdList);
        }
    };
    Submitted(p, queue, secondList, receipt);
    Check(second->signalled && !second->executing && queue.signals.size() == 1 && !p._nativeGuideFailed,
          "Signal reentry keeps stable recording snapshot across registry growth");
}
void FailureAndReplay()
{
    for (unsigned scenario = 0; scenario < 6; ++scenario)
    {
        ID3D12CommandList wrapper, native;
        ID3D12Fence fence;
        ID3D12Device device, foreignDevice;
        ID3D12CommandQueue queue, otherQueue;
        DLSSG_Dx12 p;
        p._device = &device; queue.device = &device; otherQueue.device = &device;
        auto* use = Publish(p, 1, wrapper, native, fence);
        const auto receipt = Reserve(p, queue, native);
        if (scenario == 0) queue.result = -1;
        if (scenario == 1) queue.device = &foreignDevice;
        if (scenario == 2) queue.type = 1;
        if (scenario == 3) use->value = 0;
        if (scenario == 4) use->value = UINT64_MAX;
        Submitted(p, scenario == 5 ? otherQueue : queue, wrapper, receipt);
        Check(p._nativeGuideFailed && p._managedRetained && p.deactivations == 1 && !use->signalled &&
              !p.observations && !p.NativeGuideRetired(*use), "Signal/device/queue/value failure cannot mint completion or retirement");
    }
    ID3D12CommandList wrapper, native;
    ID3D12Fence fence;
    ID3D12CommandQueue queue, replayQueue;
    DLSSG_Dx12 p;
    auto* use = Publish(p, 1, wrapper, native, fence);
    const auto receipt = Reserve(p, queue, wrapper);
    Submitted(p, queue, native, receipt);
    Reserve(p, replayQueue, native);
    Check(p._nativeGuideFailed && replayQueue.references == 1, "live alias replay retains second timeline and poisons owner");
    Submitted(p, queue, wrapper, receipt);
    p.NativeGuidesReset(&native); fence.value = use->value;
    Check(p._managedRetained && !p.NativeGuideRetired(*use), "replayed producer remains retained after Reset and completed fence");
}
void InterestKeys()
{
    ID3D12CommandList wrapper, native, replacement, unrelated;
    int keyA = 0, keyB = 0;
    dlssg::NativeGuideListInterest interest;
    Check(!interest.MayContain(&unrelated), "empty registry proves unrelated");
    interest.Publish(&keyA, &wrapper, &native);
    interest.Publish(&keyB, &wrapper, &native);
    interest.Erase(&keyA);
    Check(interest.MayContain(&wrapper) && interest.MayContain(&native), "erasing old recording retains same-list newer generation interest");
    interest.Publish(&keyB, &replacement, nullptr);
    Check(interest.MayContain(&unrelated), "incomplete pair remains conservatively interested");
    interest.Erase(&keyB);
    Check(!interest.MayContain(&wrapper), "erasing final owner removes list interest");
    interest.Publish(&keyA, &replacement, &replacement);
    std::atomic<bool> start {false}; std::atomic<unsigned> missed {0};
    ID3D12CommandList* all[] {&wrapper, &native, &replacement};
    std::thread publisher([&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (unsigned i = 0; i < 20000; ++i)
            interest.Publish(&keyA, i % 2 ? &wrapper : &replacement, i % 2 ? &native : &replacement);
    });
    start.store(true, std::memory_order_release);
    for (unsigned i = 0; i < 20000; ++i) if (!interest.MayContain(3, all)) ++missed;
    publisher.join();
    Check(missed == 0, "short-lock concurrent publication never misses known relevant batch");
    interest.Clear();
    Check(!interest.MayContain(&wrapper), "proven drain clears every interest");
}
}
int main()
{
    try
    {
        ShortLockAndReceipts(false); ShortLockAndReceipts(true);
        SixListsAndCompaction();
        Check(DS_Dx12::live == 0 && RF_Dx12::live == 0, "destroyed test provider releases all nonquarantined GPU doubles");
        SameListGenerations(false); SameListGenerations(true);
        SelectedCompletion(); MixedBatchAndClosedReplay(); CpuLeaseAndSignalReentry(); FailureAndReplay(); InterestKeys();
        std::printf("Native guide actual production methods: %u checks passed; CPU only\n", checks);
        return 0;
    }
    catch (const Failure&) { std::printf("Detected regression at check %u; CPU only\n", checks); return 42; }
}
