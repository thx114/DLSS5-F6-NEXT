// CPU-only: the vendor/device/resource boundary is fake. Router ownership,
// ContextData, the DLSS acquisition/release implementation and input gates are
// the same production code used by the Windows adapter.
#include "../SelectedFeatureRelease.h"
#include "../NativeDx12Router.h"
#include <nvsdk_ngx_params.h>
#include "../../../inputs/NVNGX_DLSS.h"
#include <ankerl/unordered_dense.h>
#include <cassert>
#include <cstdio>
#include <vector>

using Result = NVSDK_NGX_Result;
using Handle = NVSDK_NGX_Handle;
constexpr Result ok = NVSDK_NGX_Result_Success;
constexpr Result vendorFailure = NVSDK_NGX_Result_FAIL_PlatformError;
using ReleaseFunction = Result (*)(Handle*);

struct Counts
{
    int nativeCalls = 0, nativeStop = 0, selectedStop = 0, handleDeletes = 0;
    int exposureCleanup = 0, fgCleanup = 0, resets = 0, evaluations = 0;
    int destructors = 0, resourcesDestroyed = 0, paramsDestroyed = 0;
    Result destructorRelease = NVSDK_NGX_Result_Fail;
};

// Only the int payload is used, but keep the actual SDK parameter interface and
// real ContextData ParameterOwner/deleter rather than replacing its ownership.
struct Parameters final : NVSDK_NGX_Parameter
{
    Counts& counts;
    int payload = 73;
    explicit Parameters(Counts& c) : counts(c) {}
    ~Parameters() { ++counts.paramsDestroyed; }
#define PARAMETER_OVERLOAD(T) \
    void Set(const char*, T) override {} \
    Result Get(const char*, T*) const override { return NVSDK_NGX_Result_FAIL_UnsupportedParameter; }
    PARAMETER_OVERLOAD(unsigned long long)
    PARAMETER_OVERLOAD(float)
    PARAMETER_OVERLOAD(double)
    PARAMETER_OVERLOAD(unsigned int)
    PARAMETER_OVERLOAD(int)
    PARAMETER_OVERLOAD(ID3D11Resource*)
    PARAMETER_OVERLOAD(ID3D12Resource*)
    PARAMETER_OVERLOAD(void*)
#undef PARAMETER_OVERLOAD
    void Reset() override { payload = 0; }
    static void Destroy(NVSDK_NGX_Parameter* p) { delete static_cast<Parameters*>(p); }
};

struct Resources
{
    Counts& counts;
    explicit Resources(Counts& c) : counts(c) {}
    ~Resources() { ++counts.resourcesDestroyed; }
};

struct Feature
{
    Counts& counts;
    Parameters* parameters;
    std::unique_ptr<Resources> resources;
    NgxInput::NativeFeatureRelease release;
    Handle* nativeHandle = nullptr;
    ReleaseFunction callback;

    Feature(Counts& c, Parameters* p, ReleaseFunction f)
        : counts(c), parameters(p), resources(std::make_unique<Resources>(c)), callback(f) {}
    // No fake failure here: the actual production implementation invokes the
    // backend trampoline, nested Router::Release, then the vendor callback.
    Result TryRelease() { return release.TryRelease(nativeHandle, callback); }
    ~Feature()
    {
        assert(parameters && parameters->payload == 73); // parameters outlive full feature
        counts.destructorRelease = TryRelease();
        ++counts.destructors;
    }
};

struct ReleaseApi;
using Router = NgxInput::Dx12Router<ReleaseApi>;
struct ReleaseApi
{
    using Handle = ::Handle;
    using Feature = NVSDK_NGX_Feature;
    using Result = ::Result;
    Counts counts;
    Router* router = nullptr;
    ankerl::unordered_dense::map<unsigned, ContextData<::Feature>> contexts;
    std::vector<std::unique_ptr<::Feature>> delayed;
    ::Feature* currentFeature = nullptr;
    Handle backend { 0xf0012345 }; // large native Id must not imply selected ownership
    Handle* selected = nullptr;
    Result nativeResult = vendorFailure;
    Result createResult = ok;
    Result observedCreate = NVSDK_NGX_Result_Fail;
    bool outputFromCreate = true;
    bool ready = false;
    static inline thread_local ReleaseApi* active = nullptr;

    static bool Success(Result r) { return r == ok; }
    static Result Fail() { return NVSDK_NGX_Result_Fail; }
    static unsigned Id(const Handle* h) { return h->Id; }
    static bool IsSelectedFeature(Feature f) { return f == NVSDK_NGX_Feature_SuperSampling; }
    void ProviderReady(bool value) { ready = value; }
    bool AcceptInitDevice(int) { return true; }
    void ObserveInitDevice(int) {}
    void ForgetInitDevice() {}
    bool AcceptCommandDevice(int) { return true; }
    int AcquireCommandDevice(int device) { return device; }
    bool AttachInput(int) { return true; }
    void Receipt(const char*, NgxInput::Owner, const Handle*, std::optional<Feature>, std::optional<unsigned>) {}

    static Result BackendRelease(Handle* h);
    Result NativeCreate(int, Feature, NVSDK_NGX_Parameter*, Handle** out)
    {
        *out = outputFromCreate ? &backend : nullptr;
        return createResult;
    }
    Result SelectedCreate(int command, Feature f, NVSDK_NGX_Parameter*, Handle** out)
    {
        assert(!*out);
        selected = new Handle { 37 };
        auto& ctx = contexts[selected->Id];
        ctx.OwnCreateParams({ new Parameters(counts), &Parameters::Destroy });
        ctx.feature = std::make_unique<::Feature>(counts, static_cast<Parameters*>(ctx.createParams), &BackendRelease);
        auto* feature = ctx.feature.get();
        const auto result = router->Create(false, command, f, ctx.createParams, &feature->nativeHandle);
        observedCreate = feature->release.ObserveCreate(result, feature->nativeHandle);
        currentFeature = feature;
        *out = selected;
        // Real TryCreateOptiFeature retains the context and returns Success
        // even when Init fails; its next Evaluate requests a fallback switch.
        return ok;
    }
    Result NativeRelease(Handle* h)
    {
        assert(h == &backend && h != selected);
        ++counts.nativeCalls;
        return nativeResult;
    }
    Result SelectedRelease(Handle* h)
    {
        auto it = contexts.find(h->Id);
        assert(it != contexts.end());
        return NgxInput::CommitAfterFeatureRelease(true, it->second.feature.get(), [&] {
            ++counts.exposureCleanup;
            ++counts.fgCleanup;
            if (currentFeature == it->second.feature.get()) currentFeature = nullptr;
            contexts.erase(it);
            return ok;
        });
    }
    void DeleteSelectedHandle(Handle* h)
    {
        assert(h == selected);
        ++counts.handleDeletes;
        delete h;
        selected = nullptr;
    }
    Result NativeEvaluate(int, const Handle*, NVSDK_NGX_Parameter*, int) { assert(false); return Fail(); }
    Result SelectedEvaluate(int, const Handle*, NVSDK_NGX_Parameter*, int)
    {
        ++counts.evaluations;
        return ok;
    }
    Result Shutdown()
    {
        return router->Shutdown(false, [&] { ++counts.nativeStop; return ok; },
                                [&](Result r) { ++counts.selectedStop; return r; });
    }
};

Result ReleaseApi::BackendRelease(Handle* h) { return active->router->Release(false, h); }

struct Fixture
{
    ReleaseApi api;
    Router router { api };
    Handle* external = nullptr;
    Fixture() { api.router = &router; ReleaseApi::active = &api; }
    Result Create() { return router.Create(true, 1, NVSDK_NGX_Feature_SuperSampling, static_cast<NVSDK_NGX_Parameter*>(nullptr), &external); }
    // Failed partial Create tests intentionally model process-exit abandonment;
    // their assertions happen before destruction. No GPU retirement is claimed.
    ~Fixture() { delete api.selected; }
};

void NestedNativeFailureRetainsCompleteOwner()
{
    Fixture f;
    auto& a = f.api;
    assert(f.Create() == ok);
    const auto id = f.external->Id;
    auto& original = a.contexts.at(id);
    auto* const feature = original.feature.get();
    auto* const parameters = original.createParams;
    auto* const resources = feature->resources.get();
    assert(f.router.Release(true, f.external) == vendorFailure);
    auto& retained = a.contexts.at(id);
    assert(retained.feature.get() == feature && a.currentFeature == feature);
    assert(retained.createParams == parameters && retained.createParamsOwner.get() == parameters);
    assert(static_cast<Parameters*>(parameters)->payload == 73);
    assert(feature->nativeHandle == &a.backend && feature->release.Acquired());
    assert(feature->resources.get() == resources && retained.changeBackendCounter == 0);
    assert(a.counts.nativeCalls == 1 && !a.counts.exposureCleanup && !a.counts.fgCleanup);
    assert(!a.counts.destructors && !a.counts.resourcesDestroyed && !a.counts.paramsDestroyed);
    assert(!a.counts.handleDeletes && f.router.HasSelected());
    assert(a.Shutdown() != ok && !a.counts.nativeStop && !a.counts.selectedStop);
    // This is the same complete feature, not a bare native pointer in a ledger.
    assert(f.router.Evaluate(false, 1, f.external, parameters, 0) == ok);
    assert(a.counts.evaluations == 1);

    a.nativeResult = ok;
    assert(f.router.Release(false, f.external) == ok);
    assert(a.counts.nativeCalls == 2); // failure + one successful retry; destructor is idempotent
    assert(a.counts.destructors == 1 && a.counts.destructorRelease == ok);
    assert(a.counts.resourcesDestroyed == 1 && a.counts.paramsDestroyed == 1);
    assert(a.counts.exposureCleanup == 1 && a.counts.fgCleanup == 1 && a.counts.handleDeletes == 1);
    assert(a.contexts.empty() && !a.currentFeature && !f.router.HasSelected());
    assert(a.Shutdown() == ok && a.counts.nativeStop == 1 && a.counts.selectedStop == 1);
}

void MissingCallbackKeepsContextAndBlocksShutdown()
{
    Fixture f;
    auto& a = f.api;
    assert(f.Create() == ok);
    auto* feature = a.currentFeature;
    feature->callback = nullptr;
    assert(f.router.Release(true, f.external) == NVSDK_NGX_Result_FAIL_FeatureNotSupported);
    assert(a.currentFeature == feature && a.contexts.size() == 1 && feature->release.Acquired());
    assert(!a.counts.nativeCalls && !a.counts.resourcesDestroyed && !a.counts.paramsDestroyed);
    assert(a.Shutdown() != ok && !a.counts.nativeStop && f.router.HasSelected());
    feature->callback = &ReleaseApi::BackendRelease;
    a.nativeResult = ok;
    assert(f.router.Release(true, f.external) == ok && a.counts.nativeCalls == 1);
    assert(a.counts.destructorRelease == ok && !f.router.HasSelected());
    assert(a.Shutdown() == ok && a.counts.nativeStop == 1);
}

int simpleReleaseCalls = 0;
Result SimpleRelease(Handle*) { ++simpleReleaseCalls; return ok; }

void AcquisitionAndIdempotence()
{
    Handle placeholder { 0 }, real { 0xf0000001 }, unrelated { 0xf0000001 };
    Handle* published = &placeholder;
    NgxInput::NativeFeatureRelease contract;
    simpleReleaseCalls = 0;
    assert(contract.TryRelease(published, &SimpleRelease) == ok);
    assert(published == &placeholder && !contract.Acquired() && !simpleReleaseCalls);
    assert(contract.ObserveCreate(vendorFailure, nullptr) == vendorFailure);
    assert(contract.TryRelease(published, static_cast<ReleaseFunction>(nullptr)) == ok);
    assert(contract.ObserveCreate(ok, nullptr) == NVSDK_NGX_Result_Fail);
    assert(!contract.HoldsOutput());
    published = &real;
    assert(contract.ObserveCreate(ok, published) == ok && contract.Acquired());
    assert(contract.ObserveCreate(ok, &unrelated) == NVSDK_NGX_Result_FAIL_InvalidParameter);
    published = &unrelated;
    assert(contract.TryRelease(published, &SimpleRelease) == NVSDK_NGX_Result_FAIL_InvalidParameter);
    assert(published == &unrelated && contract.Acquired() && !simpleReleaseCalls);
    published = &real;
    assert(contract.TryRelease(published, static_cast<ReleaseFunction>(nullptr)) == NVSDK_NGX_Result_FAIL_FeatureNotSupported);
    assert(contract.TryRelease(published, &SimpleRelease) == ok);
    assert(!published && !contract.HoldsOutput() && simpleReleaseCalls == 1);
    assert(contract.TryRelease(published, &SimpleRelease) == ok && simpleReleaseCalls == 1);
    assert(contract.ObserveCreate(ok, &real) == ok); // a new positively observed acquisition
    published = &real;
    assert(contract.TryRelease(published, &SimpleRelease) == ok && simpleReleaseCalls == 2);
}

void FailedCreatesDoNotInventAcquisition()
{
    for (bool output : { false, true })
    {
        Fixture f;
        auto& a = f.api;
        a.createResult = vendorFailure;
        a.outputFromCreate = output;
        assert(f.Create() == ok && a.observedCreate == vendorFailure && f.router.HasSelected());
        auto* const feature = a.currentFeature;
        assert(!feature->release.Acquired() && feature->release.HoldsOutput() == output);
        if (output)
        {
            assert(f.router.Release(true, f.external) == vendorFailure);
            assert(feature == a.currentFeature && feature->nativeHandle == &a.backend);
            assert(a.contexts.size() == 1 && !a.counts.destructors && !a.counts.paramsDestroyed);
            assert(a.Shutdown() != ok && !a.counts.nativeStop);
            assert(feature->release.ObserveCreate(ok, &a.backend) == NVSDK_NGX_Result_FAIL_InvalidParameter);
            int advances = 0;
            assert(NgxInput::AdvanceAfterFeatureRelease(true, 0, feature, [&] { ++advances; return ok; }) == vendorFailure);
            assert(!advances); // Init-failure fallback cannot discard uncertain acquisition
        }
        else
        {
            assert(f.router.Release(true, f.external) == ok && !f.router.HasSelected());
            assert(a.contexts.empty() && a.counts.destructors == 1 && a.counts.paramsDestroyed == 1);
            assert(a.Shutdown() == ok && a.counts.nativeStop == 1);
        }
        assert(!a.counts.nativeCalls); // neither a failed Create nor a placeholder proves native ownership
    }
}

void InternalSwitchPreflightBeforeEveryDestructiveSideEffect()
{
    Fixture f;
    auto& a = f.api;
    assert(f.Create() == ok);
    auto& ctx = a.contexts.at(f.external->Id);
    auto* const old = ctx.feature.get();
    auto* const parameters = ctx.createParams;
    auto advance = [&] {
        ++a.counts.resets;
        ++ctx.changeBackendCounter;
        ctx.createParams = nullptr;
        a.currentFeature = nullptr;
        a.delayed.push_back(std::move(ctx.feature));
        return ok;
    };
    assert(NgxInput::AdvanceAfterFeatureRelease(true, ctx.changeBackendCounter, ctx.feature.get(), advance) == vendorFailure);
    assert(!ctx.changeBackendCounter && !a.counts.resets && a.delayed.empty());
    assert(ctx.feature.get() == old && a.currentFeature == old && old->nativeHandle == &a.backend);
    assert(ctx.createParams == parameters && ctx.createParamsOwner.get() == parameters);
    assert(!a.counts.resourcesDestroyed && !a.counts.paramsDestroyed);
    assert(a.Shutdown() != ok && !a.counts.nativeStop && f.router.HasSelected());

    a.nativeResult = ok;
    assert(NgxInput::AdvanceAfterFeatureRelease(true, ctx.changeBackendCounter, ctx.feature.get(), advance) == ok);
    assert(ctx.changeBackendCounter == 1 && a.counts.resets == 1);
    assert(!ctx.feature && !a.currentFeature && a.delayed.front().get() == old);
    assert(!old->nativeHandle && !old->release.Acquired() && a.counts.nativeCalls == 2);
    assert(!a.counts.evaluations); // phase1 ends before any old-feature Evaluate
    // Provider phase2 resets an already-empty feature. It must not re-release
    // the delayed owner or forget the router's still-live selected handle.
    assert(NgxInput::AdvanceAfterFeatureRelease(true, ctx.changeBackendCounter, ctx.feature.get(), [&] {
        ctx.feature.reset();
        ++ctx.changeBackendCounter;
        return ok;
    }) == ok);
    assert(ctx.changeBackendCounter == 2 && a.counts.nativeCalls == 2 && f.router.HasSelected());
    a.delayed.clear();
    assert(a.counts.destructors == 1 && a.counts.destructorRelease == ok && a.counts.nativeCalls == 2);
    assert(a.counts.resourcesDestroyed == 1 && !a.counts.paramsDestroyed);
    assert(f.router.Release(true, f.external) == ok); // release pending-switch empty context
    assert(a.contexts.empty() && a.counts.paramsDestroyed == 1 && !f.router.HasSelected());
    assert(a.Shutdown() == ok && a.counts.nativeStop == 1);
}

void DenseEraseMovesCompleteSurvivingContext()
{
    Counts counts;
    ankerl::unordered_dense::map<unsigned, ContextData<Feature>> contexts;
    Handle handles[] { { 900 }, { 901 } };
    for (unsigned i = 0; i != 2; ++i)
    {
        auto& ctx = contexts[i];
        ctx.OwnCreateParams({ new Parameters(counts), &Parameters::Destroy });
        ctx.feature = std::make_unique<Feature>(counts, static_cast<Parameters*>(ctx.createParams), &SimpleRelease);
        ctx.feature->nativeHandle = &handles[i];
        assert(ctx.feature->release.ObserveCreate(ok, &handles[i]) == ok);
    }
    auto* const survivor = contexts.at(1).feature.get();
    auto* const survivorParams = contexts.at(1).createParams;
    simpleReleaseCalls = 0;
    assert(NgxInput::CommitAfterFeatureRelease(true, contexts.at(0).feature.get(), [&] {
        contexts.erase(0); // unordered_dense move-assigns the last live context into the hole
        return ok;
    }) == ok);
    assert(simpleReleaseCalls == 1 && counts.destructors == 1 && counts.paramsDestroyed == 1);
    auto& retained = contexts.at(1);
    assert(retained.feature.get() == survivor && retained.createParams == survivorParams);
    assert(retained.createParamsOwner.get() == survivorParams && survivor->release.Acquired());
    assert(survivor->nativeHandle == &handles[1] && counts.resourcesDestroyed == 1);
    assert(NgxInput::CommitAfterFeatureRelease(true, survivor, [&] { contexts.clear(); return ok; }) == ok);
    assert(simpleReleaseCalls == 2 && counts.destructors == 2 && counts.paramsDestroyed == 2);
    assert(counts.resourcesDestroyed == 2 && counts.destructorRelease == ok);
}

void LegacyGateDoesNotChangeOtherInputs()
{
    Fixture f;
    assert(f.Create() == ok);
    int commits = 0;
    assert(NgxInput::CommitAfterFeatureRelease(false, f.api.currentFeature, [&] { ++commits; return ok; }) == ok);
    assert(commits == 1 && !f.api.counts.nativeCalls && f.api.currentFeature->release.Acquired());
    f.api.nativeResult = ok;
    assert(f.router.Release(true, f.external) == ok);
}

int main()
{
    NestedNativeFailureRetainsCompleteOwner();
    MissingCallbackKeepsContextAndBlocksShutdown();
    AcquisitionAndIdempotence();
    FailedCreatesDoNotInventAcquisition();
    InternalSwitchPreflightBeforeEveryDestructiveSideEffect();
    DenseEraseMovesCompleteSurvivingContext();
    LegacyGateDoesNotChangeOtherInputs();
    std::puts("PASS 7 production-release suites: nested native failure/retry, full ContextData ownership, missing callback, acquisition/idempotence, failed Create outputs, internal switch, dense erase, legacy gate");
}
