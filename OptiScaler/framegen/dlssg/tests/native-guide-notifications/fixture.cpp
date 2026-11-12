#include <framegen/dlssg/NativeGuideListInterest.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <future>
#include <mutex>
#include <thread>

using UINT = unsigned int;
using UINT64 = uint64_t;
constexpr int D3D12_COMMAND_LIST_TYPE_DIRECT = 0;
inline bool FAILED(int value) { return value < 0; }
#define LOG_INFO(...) ((void)0)
struct ID3D12CommandList {};
struct ID3D12Device {};
struct ID3D12Fence
{
    UINT64 value = 0;
    UINT64 GetCompletedValue() { return value; }
};
struct ID3D12CommandQueue
{
    unsigned signals = 0, references = 0;
    int result = 0, type = 0;
    ID3D12Device* device = nullptr;
    ID3D12Fence* lastFence = nullptr;
    UINT64 lastValue = 0;
    void AddRef() { ++references; }
    struct Desc { int Type; };
    Desc GetDesc() { return {type}; }
    int Signal(ID3D12Fence* fence, UINT64 value) { ++signals; lastFence = fence; lastValue = value; return result; }
};
namespace dlssg::depth_contract
{
inline bool OnDevice(ID3D12Device* device, ID3D12CommandQueue* queue) { return queue && device == queue->device; }
}
template<class T> struct Ptr
{
    T* ptr = nullptr;
    T* Get() const { return ptr; }
    T* operator->() const { return ptr; }
    explicit operator bool() const { return ptr != nullptr; }
    Ptr& operator=(T* value) { ptr = value; return *this; }
};
struct DLSSG_Dx12
{
    std::recursive_mutex _inputContinuityMutex;
    dlssg::NativeGuideListInterest<4> _nativeGuideListInterest;
    bool _teardownQuarantined = false, _nativeGuideFailed = false, _managedRetained = false;
    unsigned _nativeGuideSubmissions = 0, deactivations = 0, observations = 0;
    ID3D12Device* _device = nullptr;
    struct Use
    {
        Ptr<ID3D12CommandList> list, nativeList;
        Ptr<ID3D12CommandQueue> queue;
        Ptr<ID3D12Fence> fence;
        UINT64 value = 1;
        bool closed = false, submitted = false, executing = false, signalled = false;
    } _nativeGuideUses[4] {};
    bool TracksNativeGuides() const { return true; }
    void Deactivate() { ++deactivations; }
    void ObserveGuideSubmission(UINT, ID3D12Fence*, UINT64) { ++observations; }
    bool NativeGuideRetired(const Use&) const;
    void NativeGuidesSubmitting(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
    void NativeGuidesSubmitted(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
    void NativeGuidesReset(ID3D12CommandList*);
};
#define NativeGuideUse Use
#include "NativeGuideNotificationsProduction.inl"
#undef NativeGuideUse

namespace
{
unsigned checks = 0, failures = 0;
void Check(bool okay, const char* text)
{
    ++checks;
    if (!okay) { ++failures; std::printf("FAIL %s\n", text); }
}
void Publish(DLSSG_Dx12& p, ID3D12CommandList& wrapper, ID3D12CommandList& native, ID3D12Fence& fence)
{
    p._nativeGuideUses[0].list = &wrapper;
    p._nativeGuideUses[0].nativeList = &native;
    p._nativeGuideUses[0].fence = &fence;
    p._nativeGuideListInterest.Publish(0, &wrapper, &native);
}
// Always release and join even when the old implementation blocks. This is
// the source-Present/SDK-worker lock dependency, without executing vendor code.
template<class Action> bool RunsWhileContinuityHeld(DLSSG_Dx12& p, Action action)
{
    std::promise<void> entered;
    auto enteredFuture = entered.get_future();
    std::unique_lock held(p._inputContinuityMutex);
    auto worker = std::async(std::launch::async, [&] { entered.set_value(); action(); });
    enteredFuture.wait();
    const bool completed = worker.wait_for(std::chrono::milliseconds(150)) == std::future_status::ready;
    held.unlock();
    worker.get();
    return completed;
}
}

int main()
{
    DLSSG_Dx12 p;
    ID3D12CommandList wrapper, native, unrelated, replacement;
    ID3D12CommandQueue queue, wrongQueue;
    ID3D12Fence fence;
    Publish(p, wrapper, native, fence);
    ID3D12CommandList* other[] {&unrelated};
    Check(RunsWhileContinuityHeld(p, [&] { p.NativeGuidesReset(&unrelated); }), "unrelated Reset does not wait for source Present");
    Check(RunsWhileContinuityHeld(p, [&] { p.NativeGuidesSubmitting(&queue, 1, other); }), "unrelated pre-Execute does not wait for source Present");
    Check(RunsWhileContinuityHeld(p, [&] { p.NativeGuidesSubmitted(&queue, 1, other); }), "unrelated post-Execute does not wait for source Present");
    Check(!p._nativeGuideUses[0].closed && !p._nativeGuideUses[0].submitted && !queue.signals, "unrelated worker cannot mutate producer");
    Check(!RunsWhileContinuityHeld(p, [&] { p.NativeGuidesReset(&native); }), "native alias must still synchronize with source");
    Check(p._nativeGuideUses[0].closed, "native Reset closes the exact recording");
    Check(p.NativeGuideRetired(p._nativeGuideUses[0]), "unsubmitted successful Reset may discard recording");
    p._nativeGuideUses[0].closed = false;
    ID3D12CommandList* mixed[] {&unrelated, &wrapper};
    Check(!RunsWhileContinuityHeld(p, [&] { p.NativeGuidesSubmitting(&queue, 2, mixed); }), "mixed batch keeps continuity and pre-Execute reservation");
    auto& use = p._nativeGuideUses[0];
    Check(use.submitted && use.executing && use.queue.Get() == &queue, "pre-Execute reserves actual producer queue");
    p.NativeGuidesReset(&native);
    Check(!p.NativeGuideRetired(use), "Reset racing Execute is not GPU completion");
    p.NativeGuidesSubmitted(&queue, 2, mixed);
    Check(queue.signals == 1 && use.signalled && !use.executing && p.observations == 1, "post-Execute still signals after racing Reset");
    Check(queue.lastFence == &fence && queue.lastValue == use.value, "completion targets exact producer fence/value");
    Check(!p.NativeGuideRetired(use), "pending physical fence forbids retirement");
    fence.value = use.value;
    Check(p.NativeGuideRetired(use), "only actual producer fence permits retirement");
    fence.value = UINT64_MAX;
    Check(!p.NativeGuideRetired(use), "device removed fence cannot retire recording");
    fence.value = use.value;
    ID3D12CommandList* alias[] {&native};
    p.NativeGuidesSubmitting(&wrongQueue, 1, alias);
    Check(!p._nativeGuideFailed, "closed recording cannot be replayed as live reservation");
    use.closed = false;
    p.NativeGuidesSubmitting(&wrongQueue, 1, alias);
    Check(p._nativeGuideFailed && wrongQueue.references == 1, "live replay retains second timeline and poisons owner");
    p.NativeGuidesSubmitted(&queue, 1, alias);
    Check(p._managedRetained && p.deactivations == 1, "poisoned producer stays retained");

    DLSSG_Dx12 failed;
    Publish(failed, wrapper, native, fence);
    queue.result = -1;
    failed.NativeGuidesSubmitting(&queue, 1, alias);
    failed.NativeGuidesSubmitted(&queue, 1, alias);
    Check(failed._nativeGuideFailed && failed._managedRetained && !failed._nativeGuideUses[0].signalled,
          "Signal failure cannot mint completion");

    DLSSG_Dx12 multiple;
    ID3D12Fence secondFence;
    Publish(multiple, wrapper, native, fence);
    auto& second = multiple._nativeGuideUses[1];
    second.list = &replacement; second.nativeList = &replacement; second.fence = &secondFence; second.value = 7;
    multiple._nativeGuideListInterest.Publish(1, &replacement, &replacement);
    queue.result = 0;
    ID3D12CommandList* batch[] {&unrelated, &native, &replacement};
    const auto beforeSignals = queue.signals;
    multiple.NativeGuidesSubmitting(&queue, 3, batch);
    multiple.NativeGuidesReset(&wrapper);
    multiple.NativeGuidesReset(&replacement);
    multiple.NativeGuidesSubmitted(&queue, 3, batch);
    Check(queue.signals == beforeSignals + 2 && multiple.observations == 2 &&
          multiple._nativeGuideUses[0].signalled && second.signalled, "mixed multi-slot batch preserves both producer receipts");
    secondFence.value = 6;
    Check(!multiple.NativeGuideRetired(second), "second slot retains independent incomplete fence target");
    secondFence.value = 7;
    Check(multiple.NativeGuideRetired(second), "second slot retires at its own completed target");

    DLSSG_Dx12 foreign;
    ID3D12Device differentDevice;
    foreign._device = &differentDevice;
    Publish(foreign, wrapper, native, fence);
    const auto beforeForeign = queue.signals;
    foreign.NativeGuidesSubmitting(&queue, 1, alias);
    foreign.NativeGuidesSubmitted(&queue, 1, alias);
    Check(foreign._nativeGuideFailed && foreign._managedRetained && queue.signals == beforeForeign,
          "foreign device cannot signal or mint a producer receipt");

    dlssg::NativeGuideListInterest<2> interest;
    Check(!interest.MayContain(&unrelated), "empty unpublished slots prove unrelated");
    interest.Publish(0, &wrapper, &native);
    Check(interest.MayContain(&wrapper) && interest.MayContain(&native), "both stable aliases match");
    interest.Publish(1, &replacement, nullptr);
    Check(interest.MayContain(&unrelated), "incomplete published pair is unknown");
    interest.Clear();
    Check(!interest.MayContain(&wrapper), "safe drain clears old identity");
    interest.Publish(0, &replacement, &replacement);
    Check(!interest.MayContain(&wrapper) && interest.MayContain(&replacement), "retired slot replacement does not borrow old identity");

    std::atomic<bool> start {false};
    std::atomic<unsigned> missed {0};
    ID3D12CommandList* all[] {&wrapper, &native, &replacement};
    std::thread publisher([&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (unsigned i = 0; i < 20000; ++i)
            interest.Publish(0, i % 2 ? &wrapper : &replacement, i % 2 ? &native : &replacement);
    });
    start.store(true, std::memory_order_release);
    for (unsigned i = 0; i < 20000; ++i) if (!interest.MayContain(3, all)) ++missed;
    publisher.join();
    Check(missed == 0, "concurrent publication keeps a known relevant batch interested");
    std::printf("Native guide production notifications: %u checks, %u failures; CPU only\n", checks, failures);
    return failures ? 42 : 0;
}
