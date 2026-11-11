#include "../NativeDx12Router.h"
#include <cassert>
#include <cstdio>
#include <functional>
#include <future>
#include <vector>

using NgxInput::Owner;
struct MockApi
{
    struct Handle { unsigned id; };
    using Feature = int;
    using Result = int;
    int nativeCreates = 0, selectedCreates = 0, nativeEvals = 0, selectedEvals = 0;
    int nativeReleases = 0, selectedReleases = 0, deleted = 0, attaches = 0;
    int allocations = 0, parameterDestroys = 0, pendingParameter = 0;
    int initDevice = 0, inputDevice = 0;
    bool ready = false, nativeSession = true, attachAllowed = true, destroyAllowed = true;
    bool failCreate = false, partialCreate = false, failRelease = false, failEval = false;
    Handle* forcedOutput = nullptr;
    std::vector<Handle*> allocationsToFree;
    std::function<void()> nativeCallback;
    std::function<void()> selectedCallback;
    struct ReceiptData { Owner owner; const Handle* handle; std::optional<int> feature; std::optional<unsigned> id; };
    std::vector<ReceiptData> receipts;

    ~MockApi() { for (auto* h : allocationsToFree) delete h; }
    static bool Success(int result) { return result == 1; }
    static int Ok() { return 1; }
    static int Fail() { return -1; }
    static unsigned Id(const Handle* handle) { return handle->id; }
    static bool IsSelectedFeature(int feature) { return feature == 1; }
    void ProviderReady(bool value) { ready = value; }
    bool AcceptInitDevice(int device) { return device && (!initDevice || initDevice == device) && (!inputDevice || inputDevice == device); }
    void ObserveInitDevice(int device) { initDevice = device; }
    void ForgetInitDevice() { initDevice = inputDevice = pendingParameter = 0; }
    bool AcceptCommandDevice(int device) { return AcceptInitDevice(device); }
    bool AttachInput(int device)
    {
        ++attaches;
        if (!AcceptCommandDevice(device) || !nativeSession) return false;
        if (!pendingParameter) { ++allocations; pendingParameter = 1; }
        ++parameterDestroys;
        if (!destroyAllowed) return false;
        pendingParameter = 0;
        if (!attachAllowed) return false;
        inputDevice = device;
        ready = true;
        return true;
    }
    void Receipt(const char*, Owner owner, const Handle* handle, std::optional<int> feature, std::optional<unsigned> id)
    { receipts.push_back({ owner, handle, feature, id }); }
    Handle* New(unsigned id)
    {
        auto* handle = new Handle { id };
        allocationsToFree.push_back(handle);
        return handle;
    }
    int NativeCreate(int, int, int*, Handle** out)
    {
        ++nativeCreates;
        if (const auto callback = nativeCallback) callback();
        if (!out) return -2;
        *out = forcedOutput ? forcedOutput : New(0xf0001234); // deliberately in the Opti-looking numeric range
        return 1;
    }
    int SelectedCreate(int, int, int*, Handle** out)
    {
        ++selectedCreates;
        if (const auto callback = selectedCallback) callback();
        // Reproduce the actual input's dangerous existing-*out behavior. The
        // router must protect an old native handle without changing this mock.
        if (*out) (*out)->id = 999;
        if (failCreate && !partialCreate) return -3;
        *out = forcedOutput ? forcedOutput : New(42);
        return failCreate ? -3 : 1;
    }
    int NativeEvaluate(int, const Handle*, int*, int)
    {
        ++nativeEvals;
        if (const auto callback = nativeCallback) callback();
        return 1;
    }
    int SelectedEvaluate(int, const Handle*, int*, int)
    {
        ++selectedEvals;
        if (const auto callback = selectedCallback) callback();
        return failEval ? -4 : 1;
    }
    int NativeRelease(Handle*) { ++nativeReleases; return failRelease ? -5 : 1; }
    int SelectedRelease(Handle*) { ++selectedReleases; return failRelease ? -5 : 1; }
    void DeleteSelectedHandle(Handle*) { ++deleted; } // retain memory for address-reuse assertions
};
using Router = NgxInput::Dx12Router<MockApi>;
using Handle = MockApi::Handle;
int parameters = 0;

void PointerIdentityAndUnknownCalls()
{
    MockApi a;
    Router r(a);
    Handle oldNative { 0xf1234567 };
    Handle* selected = &oldNative;
    assert(r.Create(true, 7, 1, &parameters, &selected) == 1);
    assert(oldNative.id == 0xf1234567 && selected != &oldNative);
    assert(a.nativeCreates == 0 && a.nativeReleases == 0); // no GPU witness Create/Release
    assert(a.allocations == 1 && a.parameterDestroys == 1);
    assert(r.Evaluate(false, 7, selected, &parameters, 0) == 1); // unknown caller, known selected pointer
    assert(a.selectedEvals == 1 && a.nativeEvals == 0);
    Handle collision { selected->id };
    assert(r.Evaluate(true, 7, &collision, &parameters, 0) == 1);
    assert(r.Evaluate(true, 7, &oldNative, &parameters, 0) == 1);
    assert(a.nativeEvals == 2);
    assert(a.receipts.back().owner == Owner::Unknown && !a.receipts.back().feature && !a.receipts.back().id);
    a.failEval = true;
    assert(r.Evaluate(true, 7, selected, &parameters, 0) == -4);
    assert(a.selectedEvals == 2 && a.nativeEvals == 2); // no native fallback/double Evaluate
    assert(r.Release(false, selected) == 1);
    assert(a.deleted == 1 && !r.HasSelected());
    assert(r.Release(true, &oldNative) == 1);
    assert(a.nativeReleases == 1);
}

void NativeAndBackendDoNotBecomeInputs()
{
    MockApi a;
    Router r(a);
    Handle* native = nullptr;
    assert(r.Create(false, 1, 1, &parameters, &native) == 1);
    assert(a.selectedCreates == 0 && a.attaches == 0);
    assert(a.receipts.back().owner == Owner::Backend && a.receipts.back().feature == 1);
    assert(r.Evaluate(true, 1, native, &parameters, 0) == 1);
    assert(a.nativeEvals == 1 && a.selectedEvals == 0);
    Handle* feature11 = nullptr;
    assert(r.Create(true, 1, 11, &parameters, &feature11) == 1);
    assert(a.receipts.back().feature == 11 && a.receipts.back().owner == Owner::Native);
    assert(r.Release(false, feature11) == 1);
    assert(r.Release(true, native) == 1);
    assert(a.deleted == 0);
}

void InitAndShutdownResults()
{
    MockApi a;
    Router r(a);
    int nativeInit = 0, selectedInit = 0, nativeStop = 0, selectedStop = 0;
    auto start = [&] { ++nativeInit; return -7; };
    auto attach = [&](int) { ++selectedInit; return 1; };
    assert(r.Init(true, 4, start, attach) == -7);
    assert(nativeInit == 1 && selectedInit == 0 && !a.ready);
    assert(r.Init(true, 4, [&] { ++nativeInit; return 1; }, [&](int result) { ++selectedInit; assert(result == 1); return -8; }) == 1);
    assert(a.ready && nativeInit == 2); // optional attach does not fake Init failure
    assert(r.Init(true, 5, [&] { ++nativeInit; return 1; }, attach) < 0);
    assert(nativeInit == 2); // reject differing known device BEFORE native side effects
    Handle* h = nullptr;
    assert(r.Create(true, 4, 1, &parameters, &h) == 1);
    assert(r.Init(false, 4, [&] { ++nativeInit; return 1; }, attach) < 0);
    auto stop = [&] { ++nativeStop; return 1; };
    auto cleanup = [&](int result) { ++selectedStop; assert(result == 1); return 1; };
    assert(r.Shutdown(false, stop, cleanup) < 0 && nativeStop == 0);
    a.failRelease = true;
    assert(r.Release(true, h) == -5 && r.HasSelected() && a.deleted == 0);
    assert(r.Evaluate(false, 4, h, &parameters, 0) == 1);
    a.failRelease = false;
    assert(r.Release(false, h) == 1);
    assert(r.Shutdown(true, [&] { ++nativeStop; return -9; }, cleanup) == -9);
    assert(selectedStop == 0 && a.ready);
    assert(r.Shutdown(false, stop, cleanup) == 1);
    assert(nativeStop == 2 && selectedStop == 1 && !a.ready && a.initDevice == 0);
}

void LateProbeAndPartialFailure()
{
    MockApi a;
    Router r(a);
    a.destroyAllowed = false;
    Handle* h = nullptr;
    assert(r.Create(true, 9, 1, &parameters, &h) == 1);
    assert(a.nativeCreates == 1 && a.selectedCreates == 0 && a.allocations == 1);
    assert(r.Release(true, h) == 1);
    assert(r.Create(true, 9, 1, &parameters, &h) == 1);
    assert(a.allocations == 1); // bounded retained native parameter ownership
    assert(r.Release(true, h) == 1);
    a.destroyAllowed = true;
    a.failCreate = true;
    Handle old { 123 };
    h = &old;
    assert(r.Create(true, 9, 1, &parameters, &h) == -3);
    assert(h == &old && old.id == 123 && a.nativeCreates == 2);
    a.partialCreate = true;
    assert(r.Create(true, 9, 1, &parameters, &h) == -3);
    assert(h != &old && r.HasSelected());
    assert(a.selectedReleases == 0); // no unsafe immediate GPU cleanup on failure
    assert(r.Release(false, h) == 1 && !r.HasSelected());
}

void RepeatedAndConflictingCreates()
{
    MockApi a;
    Router r(a);
    Handle* h = nullptr;
    assert(r.Create(true, 6, 1, &parameters, &h) == 1);
    a.forcedOutput = h;
    Handle* again = nullptr;
    assert(r.Create(true, 6, 1, &parameters, &again) == 1); // exact idempotent insert
    assert(again == h);
    assert(r.Evaluate(false, 6, h, &parameters, 0) == 1 && a.selectedEvals == 1);
    assert(r.Release(false, h) == 1 && !r.HasSelected() && a.deleted == 1);
    // A positively observed Create AFTER Release may reuse the same address,
    // including the same id, without inheriting the old selected owner.
    assert(r.Create(false, 6, 11, &parameters, &again) == 1);
    assert(r.Evaluate(true, 6, again, &parameters, 0) == 1 && a.nativeEvals == 1);
    assert(r.Release(true, again) == 1);
}

void ConflictingCreatesQuarantineBothDirections()
{
    for (int conflict = 0; conflict != 4; ++conflict)
    {
        MockApi a;
        Router r(a);
        Handle* h = nullptr;
        const bool firstSelected = conflict != 3;
        assert(r.Create(firstSelected, 6, 1, &parameters, &h) == 1);
        a.forcedOutput = h;
        if (conflict == 2) ++h->id; // same-address NEW Create with changed id
        Handle* collision = nullptr;
        const int result = conflict == 3 ? r.Create(true, 6, 1, &parameters, &collision) :
                           conflict == 1 ? r.Create(true, 6, 11, &parameters, &collision) :
                                           r.Create(false, 6, 1, &parameters, &collision);
        assert(result < 0);
        const auto nativeEvals = a.nativeEvals, selectedEvals = a.selectedEvals;
        const auto nativeReleases = a.nativeReleases, selectedReleases = a.selectedReleases;
        assert(r.Evaluate(false, 6, h, &parameters, 0) < 0);
        assert(r.Release(true, h) < 0);
        assert(a.nativeEvals == nativeEvals && a.selectedEvals == selectedEvals);
        assert(a.nativeReleases == nativeReleases && a.selectedReleases == selectedReleases && a.deleted == 0);
        int nativeStops = 0;
        assert(r.Shutdown(true, [&] { ++nativeStops; return 1; }, [](int r) { return r; }) < 0);
        assert(nativeStops == 0 && r.HasSelected()); // reverse collision also retains new selected owner
    }
}

void ChangedIdCannotResurrectAStaleOwner()
{
    for (bool releaseFirst : {false, true})
    {
        MockApi a;
        Router r(a);
        Handle* h = nullptr;
        assert(r.Create(true, 6, 1, &parameters, &h) == 1);
        const auto id = h->id;
        ++h->id;
        if (releaseFirst) assert(r.Release(false, h) < 0);
        else assert(r.Evaluate(false, 6, h, &parameters, 0) < 0);
        h->id = id;
        assert(r.Evaluate(true, 6, h, &parameters, 0) < 0);
        assert(r.Release(true, h) < 0);
        assert(a.nativeEvals == 0 && a.selectedEvals == 0 && a.nativeReleases == 0 && a.selectedReleases == 0);
        assert(r.HasSelected() && a.deleted == 0);
    }
}

void NativeWorkerCanReenterDuringCreate()
{
    MockApi a;
    Router r(a);
    Handle preHook { 0xffffffff };
    a.nativeCallback = [&] {
        // Native Create may wait for another native thread. That thread's
        // handle lookup cannot require the selected input operation lock.
        auto worker = std::async(std::launch::async, [&] {
            a.nativeCallback = nullptr;
            return r.Evaluate(false, 1, &preHook, &parameters, 0);
        });
        assert(worker.get() == 1);
    };
    Handle* h = nullptr;
    assert(r.Create(false, 1, 1, &parameters, &h) == 1);
    assert(a.nativeEvals == 1 && a.selectedEvals == 0);
    assert(r.Release(false, h) == 1);
}

void NativeWorkerInitCannotWaitForItsSelectedCaller()
{
    MockApi a;
    Router r(a);
    int nativeInits = 0;
    a.selectedCallback = [&] {
        auto worker = std::async(std::launch::async, [&] {
            return r.Init(false, 6, [&] { ++nativeInits; return 1; }, [](int result) { return result; });
        });
        assert(worker.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        assert(worker.get() < 0);
    };
    Handle* h = nullptr;
    assert(r.Create(true, 6, 1, &parameters, &h) == 1); // selectedCalls busy, selectedCount still zero
    assert(r.Evaluate(false, 6, h, &parameters, 0) == 1); // known selected owner in flight
    assert(nativeInits == 0);
    a.selectedCallback = nullptr;
    assert(r.Release(false, h) == 1);
}

int main()
{
    PointerIdentityAndUnknownCalls();
    NativeAndBackendDoNotBecomeInputs();
    InitAndShutdownResults();
    LateProbeAndPartialFailure();
    RepeatedAndConflictingCreates();
    ConflictingCreatesQuarantineBothDirections();
    ChangedIdCannotResurrectAStaleOwner();
    NativeWorkerCanReenterDuringCreate();
    NativeWorkerInitCannotWaitForItsSelectedCaller();
    std::puts("PASS 9 production-router suites: pointer/id, ownership, lifecycle, failure, late CPU probe, conflicts in both directions, native worker Init");
}
