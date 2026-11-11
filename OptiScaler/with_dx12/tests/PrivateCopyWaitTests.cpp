#include <Windows.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <stdexcept>
#include <thread>
#include <vector>

// CPU stand-ins expose the actual queue submission count and COM pins. The
// generated header below contains the production receipt, TLS scope and adapter.
// These stand-ins do not execute a D3D queue or establish GPU completion.
struct ID3D12Fence
{
    ULONG references = 1;
    ULONG AddRef() { return ++references; }
    ULONG Release() { return --references; }
};

struct ID3D12CommandQueue
{
    struct Call { ID3D12Fence* fence; UINT64 value; };
    ULONG references = 1;
    HRESULT result = S_OK;
    std::vector<Call> calls;
    std::function<void()> callback;
    ULONG AddRef() { return ++references; }
    ULONG Release() { return --references; }
    HRESULT Wait(ID3D12Fence* fence, UINT64 value)
    {
        calls.push_back({ fence, value });
        if (callback) callback();
        return result;
    }
};

#include "PrivateCopyWaitProduction.h"

namespace
{
unsigned checks = 0;
void Check(bool condition, const char* message)
{
    ++checks;
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

void ExactSameWriterSubmitsOnce()
{
    int owner = 0;
    ID3D12CommandQueue queue;
    ID3D12Fence fence;
    {
        ScopedPrivateCopySlot copy(&owner, 2);
        Check(ResolvePrivateCopySlot(&owner, 0) == 2, "scope still selects its exact private slot");
        Check(OrderPrivateCopyWait(&owner, &queue, &fence, 41) == S_OK, "producer handoff submits");
        Check(OrderPrivateCopyWait(&owner, &queue, &fence, 41) == S_OK, "same color writer inherits queue order");
        Check(queue.calls.size() == 1 && queue.calls[0].fence == &fence && queue.calls[0].value == 41,
              "one actual Wait covers the same queue, fence and value");
        Check(queue.references == 2 && fence.references == 2, "receipt pins both comparison owners");
    }
    Check(!activePrivateCopySlot && queue.references == 1 && fence.references == 1,
          "receipt and its COM pins end with the private transaction");
    {
        ScopedPrivateCopySlot next(&owner, 2);
        Check(OrderPrivateCopyWait(&owner, &queue, &fence, 41) == S_OK && queue.calls.size() == 2,
              "a later transaction cannot inherit an earlier submission receipt");
    }
}

void EveryQueueFenceAndWriterBoundarySubmits()
{
    int owner = 0;
    ID3D12CommandQueue producer, presenting;
    ID3D12Fence first, second;
    ScopedPrivateCopySlot copy(&owner, 1);
    Check(OrderPrivateCopyWait(&owner, &producer, &first, 10) == S_OK, "producer queue submits");
    Check(OrderPrivateCopyWait(&owner, &presenting, &first, 10) == S_OK && presenting.calls.size() == 1,
          "a different presenting queue must submit its own Wait");
    Check(OrderPrivateCopyWait(&owner, &presenting, &second, 10) == S_OK && presenting.calls.size() == 2,
          "another fence is not covered by an equal numeric value");
    Check(OrderPrivateCopyWait(&owner, &presenting, &second, 11) == S_OK && presenting.calls.size() == 3,
          "the next writer needs a new queue submission");
    Check(OrderPrivateCopyWait(&owner, &presenting, &second, 10) == S_OK && presenting.calls.size() == 4,
          "numeric inequality alone never grants order");
    Check(OrderPrivateCopyWait(&owner, &presenting, &second, 0) == E_INVALIDARG, "zero writer is invalid");
    Check(OrderPrivateCopyWait(&owner, &presenting, &second, UINT64_MAX) == E_INVALIDARG, "exhausted writer is invalid");
    Check(OrderPrivateCopyWait(&owner, &presenting, &second, 10) == S_OK && presenting.calls.size() == 5,
          "invalid writers revoke the prior submission receipt");
}

void ScopesDoNotLeakIntoOtherOwnersOrLegacyCalls()
{
    int owner = 0, other = 0;
    ID3D12CommandQueue queue;
    ID3D12Fence fence;
    Check(OrderPrivateCopyWait(&owner, &queue, &fence, 9) == S_OK, "unscoped legacy wait submits");
    Check(OrderPrivateCopyWait(&owner, &queue, &fence, 9) == S_OK && queue.calls.size() == 2,
          "generic interop never caches a Wait");
    {
        ScopedPrivateCopySlot copy(&owner, 2);
        Check(OrderPrivateCopyWait(&owner, &queue, &fence, 9) == S_OK, "outer private copy submits");
        Check(OrderPrivateCopyWait(&other, &queue, &fence, 9) == S_OK && queue.calls.size() == 4,
              "another wrapper cannot borrow the active copy's receipt");
        {
            ScopedPrivateCopySlot nested(&other, 3);
            Check(ResolvePrivateCopySlot(&owner, 0) == 0 && ResolvePrivateCopySlot(&other, 0) == 3,
                  "nested owner sees only its own private slot");
            Check(OrderPrivateCopyWait(&other, &queue, &fence, 9) == S_OK && queue.calls.size() == 5,
                  "a nested copy starts with no receipt");
        }
        Check(ResolvePrivateCopySlot(&owner, 0) == 2, "nested scope restores its predecessor");
        Check(OrderPrivateCopyWait(&owner, &queue, &fence, 9) == S_OK && queue.calls.size() == 5,
              "additional later work cannot undo the outer copy's already submitted order");
        Check(OrderPrivateCopyWait(&owner, &queue, &fence, 10) == S_OK && queue.calls.size() == 6,
              "a lifecycle handoff's fresh Signal value always submits a real Wait");
    }
    Check(OrderPrivateCopyWait(&owner, &queue, &fence, 10) == S_OK && queue.calls.size() == 7,
          "unscoped lifecycle does not consume a departed private receipt");
}

void FailuresReentryAndExceptionsCannotCertifyOrder()
{
    int owner = 0;
    ID3D12CommandQueue queue;
    ID3D12Fence fence;
    ScopedPrivateCopySlot copy(&owner, 0);
    queue.result = E_FAIL;
    Check(OrderPrivateCopyWait(&owner, &queue, &fence, 20) == E_FAIL, "actual failure is preserved");
    Check(OrderPrivateCopyWait(&owner, &queue, &fence, 20) == E_FAIL && queue.calls.size() == 2,
          "a failed Wait is never cached");
    queue.result = S_FALSE;
    Check(OrderPrivateCopyWait(&owner, &queue, &fence, 20) == S_FALSE, "successful HRESULT remains exact");
    Check(OrderPrivateCopyWait(&owner, &queue, &fence, 20) == S_FALSE && queue.calls.size() == 3,
          "reuse does not turn the actual HRESULT into a synthetic S_OK");
    queue.result = S_OK;
    queue.callback = [&] {
        Check(OrderPrivateCopyWait(&owner, &queue, &fence, 21) == E_UNEXPECTED,
              "recursive submission fails without entering the queue again");
    };
    Check(OrderPrivateCopyWait(&owner, &queue, &fence, 21) == E_UNEXPECTED && queue.calls.size() == 4,
          "an outer success cannot certify a recursively superseded transaction");
    queue.callback = {};
    Check(OrderPrivateCopyWait(&owner, &queue, &fence, 21) == S_OK && queue.calls.size() == 5,
          "the next ordinary call retries after recursion");
    queue.callback = [] { throw std::runtime_error("synthetic Wait failure"); };
    bool threw = false;
    try { static_cast<void>(OrderPrivateCopyWait(&owner, &queue, &fence, 22)); }
    catch (const std::runtime_error&) { threw = true; }
    Check(threw, "submission exceptions propagate");
    queue.callback = {};
    Check(OrderPrivateCopyWait(&owner, &queue, &fence, 22) == S_OK && queue.calls.size() == 7,
          "exception cleanup cannot leave a cached success or a stuck submission guard");
}

void DifferentQueueReentryCannotReleaseTheOuterCall()
{
    int owner = 0;
    ID3D12CommandQueue outer, nested;
    ID3D12Fence first, second;
    {
        ScopedPrivateCopySlot copy(&owner, 1);
        outer.callback = [&] {
            Check(outer.references >= 2 && first.references >= 2,
                  "outer call owns queue and fence while Wait is executing");
            Check(OrderPrivateCopyWait(&owner, &nested, &second, 61) == E_UNEXPECTED,
                  "different-queue reentry refuses before replacing owners");
            Check(outer.references >= 2 && first.references >= 2,
                  "refused reentry cannot drop an executing call's pins");
            Check(nested.references == 1 && second.references == 1 && nested.calls.empty(),
                  "refused reentry does not publish or submit the nested owners");
        };
        Check(OrderPrivateCopyWait(&owner, &outer, &first, 60) == E_UNEXPECTED,
              "a reentered outer success cannot certify order");
        outer.callback = {};
        Check(OrderPrivateCopyWait(&owner, &outer, &first, 60) == S_OK && outer.calls.size() == 2,
              "outer submission retries after refused different-queue reentry");
    }
    Check(outer.references == 1 && first.references == 1 && nested.references == 1 && second.references == 1,
          "all call-local and receipt pins end with their actual scopes");
}

void AnotherThreadCannotBorrowTheSourceSubmission()
{
    int owner = 0;
    ID3D12CommandQueue queue;
    ID3D12Fence fence;
    ScopedPrivateCopySlot source(&owner, 2);
    Check(OrderPrivateCopyWait(&owner, &queue, &fence, 30) == S_OK, "source thread submits its own handoff");
    std::thread worker([&] {
        Check(!activePrivateCopySlot, "another thread cannot observe the source's private scope");
        ScopedPrivateCopySlot copy(&owner, 2);
        Check(OrderPrivateCopyWait(&owner, &queue, &fence, 30) == S_OK,
              "another thread submits a separate handoff for the same numeric tuple");
    });
    worker.join(); // Stand-in queue/ref counters are accessed sequentially across this join.
    Check(queue.calls.size() == 2 && queue.references == 2 && fence.references == 2,
          "worker receipt leaves no shared acknowledgement or extra COM pin");
    Check(OrderPrivateCopyWait(&owner, &queue, &fence, 30) == S_OK && queue.calls.size() == 2,
          "source thread keeps only its own earlier submission receipt");
}
}

int main()
{
    ExactSameWriterSubmitsOnce();
    EveryQueueFenceAndWriterBoundarySubmits();
    ScopesDoNotLeakIntoOtherOwnersOrLegacyCalls();
    FailuresReentryAndExceptionsCannotCertifyOrder();
    DifferentQueueReentryCannotReleaseTheOuterCall();
    AnotherThreadCannotBorrowTheSourceSubmission();
    std::printf("PASS %u private-copy submission CPU checks; D3D/SDK/GPU/FPS untested\n", checks);
}
