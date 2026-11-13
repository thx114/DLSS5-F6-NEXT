// Standalone CPU contract tests. Include the production policy and public SDK
// types; only submission callbacks are fake. No SDK library/device is loaded.
// Lead can compile with C++20 /EHsc /W4 /WX and external/streamline on the include path.
#include "../DlssgOptionsPolicy.h"
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace
{
using namespace dlssg::options;
unsigned checks = 0;
void Check(bool value, int line)
{
    ++checks;
    if (!value) { std::fprintf(stderr, "contract failed at line %d\n", line); std::abort(); }
}
#define CHECK(value) Check(static_cast<bool>(value), __LINE__)

ProviderOwner Owner()
{
    return { 1, 1, 0x100, 0x200, 0x300, 0x400, 0x500, 0x600, 0x700, 0, 1920, 1080 };
}

Scope Context()
{
    Scope result {};
    result.provider = Owner();
    result.input.device = result.provider.device;
    result.input.queue = result.provider.queue;
    result.input.epoch = 1;
    result.sourceGeneration = 1;
    result.reflexPlugin = 0x800;
    result.reflexSetOptions = 0x900;
    result.enabled = true;
    return result;
}

sl::DLSSGOptions On(uint32_t generated = 1)
{
    sl::DLSSGOptions result {};
    result.mode = sl::DLSSGMode::eOn;
    result.numFramesToGenerate = generated;
    result.queueParallelismMode = sl::DLSSGQueueParallelismMode::eBlockPresentingClientQueue;
    return result;
}

sl::ReflexOptions Reflex()
{
    sl::ReflexOptions result {};
    result.mode = sl::ReflexMode::eLowLatency;
    return result;
}

struct Fixture
{
    ConfigurationState state;
    Acknowledgement<sl::DLSSGOptions> fg;
    Acknowledgement<sl::ReflexOptions> reflex;
    Scope context = Context();
    uint64_t frame = 1;
    unsigned calls = 0;
    bool eligible = true, current = true;
    sl::Result nextResult = sl::Result::eOk;
    std::function<void()> callback;

    template<typename T> Submission Apply(Acknowledgement<T>& ack, const T& value, bool force = false)
    {
        const auto before = context;
        const auto beforeFrame = frame;
        return state.Apply(ack, before, value, frame, eligible, force,
            [&] { return current && before == context && beforeFrame == frame; },
            [&] {
                ++calls;
                if (callback) callback();
                return nextResult;
            });
    }
    Submission Apply(const sl::DLSSGOptions& value, bool force = false)
    {
        return Apply(fg, value, force);
    }
};

void SuccessfulAcknowledgementIsReusedAcrossSlots()
{
    Fixture f;
    CHECK(f.Apply(On()).submitted);
    CHECK(f.Apply(f.reflex, Reflex()).submitted);
    const auto calls = f.calls;
    // Different real sources/ring slots do not change a persistent SDK option.
    for (uint64_t frame = 2; frame != 32; ++frame)
    {
        f.frame = frame;
        const auto fg = f.Apply(On());
        const auto reflex = f.Apply(f.reflex, Reflex());
        CHECK(fg.confirmed && !fg.submitted && reflex.confirmed && !reflex.submitted);
    }
    CHECK(f.calls == calls);
    CHECK(f.Apply(On(5)).submitted);
    CHECK(f.Apply(On(5)).confirmed);
}

void UnownedSdkRequiresARealCallEverySource()
{
    Fixture f;
    f.eligible = false;
    for (unsigned repeat = 0; repeat != 3; ++repeat)
    {
        CHECK(f.Apply(On(3)).submitted);
        CHECK(f.Apply(f.reflex, Reflex()).submitted);
    }
    CHECK(f.calls == 6);
}

void SixfoldSourcesReuseOptionsButFreshGapRecoveryReallyReenables()
{
    Fixture f;
    GenerationState generation;
    sl::DLSSGState raw {};
    raw.numFramesToGenerateMax = 5;
    generation.Observe(f.context.provider, raw, sl::Result::eOk, true);
    const auto options = On(5);
    const auto selection = generation.Select(f.context.provider, 5, false);
    for (f.frame = 1; f.frame <= 1800; ++f.frame)
    {
        const auto outcome = f.Apply(options);
        generation.Record(f.context, f.frame, selection, options, outcome);
        CHECK(generation.Accepted(f.context, f.frame));
        CHECK(!generation.Accepted(f.context, f.frame + 1));
        CHECK(generation.FixedTotal() == 6);
        CHECK(f.Apply(f.reflex, Reflex()).confirmed);
    }
    CHECK(f.calls == 6); // One pair per 600 real sources, not one per frame/slot.
    auto off = options;
    off.mode = sl::DLSSGMode::eOff;
    off.flags |= sl::DLSSGFlags::eRetainResourcesWhenOff;
    for (unsigned gap = 0; gap != 59; ++gap)
    {
        const auto before = f.calls;
        const auto stopped = f.state.Release(off, [&] { return f.current; }, [&] {
            ++f.calls;
            return sl::Result::eOk;
        });
        CHECK(stopped.submitted && stopped.confirmed);
        ++f.frame;
        const auto resumed = f.Apply(options, true);
        CHECK(resumed.submitted && resumed.confirmed);
        generation.Record(f.context, f.frame, selection, options, resumed);
        CHECK(generation.Accepted(f.context, f.frame) && generation.FixedTotal() == 6);
        CHECK(f.Apply(f.reflex, Reflex(), true).submitted);
        ++f.frame;
        CHECK(!f.Apply(options).submitted);
        CHECK(!f.Apply(f.reflex, Reflex()).submitted);
        CHECK(f.calls == before + 3); // Off, fresh On and fresh Reflex only.
    }
}

void KeepaliveUsesSourceAgeAndRetriesWarnings()
{
    Fixture f;
    CHECK(f.Apply(On()).confirmed);
    for (unsigned repeat = 0; repeat != 700; ++repeat) CHECK(!f.Apply(On()).submitted);
    f.frame = 600;
    CHECK(!f.Apply(On()).submitted); // Age 599, not 600 calls.
    f.frame = 601;
    f.nextResult = sl::Result::eWarnOutOfVRAM;
    const auto warning = f.Apply(On());
    CHECK(warning.submitted && !warning.confirmed && warning.Result() == sl::Result::eWarnOutOfVRAM);
    CHECK(f.Apply(On()).submitted); // Same-frame failure is never a hit.
    f.nextResult = sl::Result::eOk;
    CHECK(f.Apply(On()).submitted);
    CHECK(!f.Apply(On()).submitted);
    CHECK(f.Apply(On(), true).submitted); // Explicit resume cannot inherit an On hit.
}

void RetainedFgGapsPreserveReflexScopeAndKeepalive()
{
    Fixture f;
    CHECK(f.Apply(On(5)).confirmed);
    CHECK(f.Apply(f.reflex, Reflex()).confirmed);
    const auto reflexEpoch = f.reflex.epoch;
    const auto reflexFrame = f.reflex.sourceFrame;
    // Match the root adapter: successful SDK Off+retain leaves Reflex running,
    // then revokes only FG. Generic Release/real Off still revokes both.
    for (unsigned gap = 0; gap != 37; ++gap)
    {
        const auto before = f.calls;
        ++f.calls; // Successful direct SDK FG Off+retain, no Reflex call.
        const auto serial = f.state.Serial();
        f.state.InvalidateDlssg();
        f.fg.valid = false; // Root also clears its concrete FG ACK, never Reflex.
        CHECK(f.state.Serial() > serial);
        ++f.frame;
        const auto resumed = f.Apply(On(5));
        CHECK(resumed.submitted && resumed.confirmed && resumed.reason == Reason::Invalidated);
        const auto retained = f.Apply(f.reflex, Reflex());
        CHECK(!retained.submitted && retained.confirmed && retained.reason == Reason::AckHit);
        CHECK(f.reflex.epoch == reflexEpoch && f.reflex.sourceFrame == reflexFrame);
        CHECK(f.calls == before + 2); // Only Off and fresh FG On.
    }
    f.frame = 600;
    CHECK(f.Apply(f.reflex, Reflex()).reason == Reason::AckHit);
    f.state.InvalidateDlssg();
    f.frame = 601; // Gaps must not reset the original Reflex keepalive clock.
    const auto refresh = f.Apply(f.reflex, Reflex());
    CHECK(refresh.submitted && refresh.confirmed && refresh.reason == Reason::Keepalive);
    CHECK(f.Apply(On(5)).submitted);
    CHECK(f.Apply(f.reflex, Reflex()).reason == Reason::AckHit);
    f.frame = 1200;
    CHECK(f.Apply(f.reflex, Reflex()).reason == Reason::AckHit);
    f.frame = 1201;
    CHECK(f.Apply(f.reflex, Reflex()).reason == Reason::Keepalive);

    // Real teardown may also request retained resources. It explicitly closes
    // both ACK lifetimes before SDK Off, unlike the successful FG-only gap.
    f.state.Invalidate();
    auto off = On(5);
    off.mode = sl::DLSSGMode::eOff;
    off.flags |= sl::DLSSGFlags::eRetainResourcesWhenOff;
    CHECK(f.state.Release(off, [] { return true; }, [] { return sl::Result::eOk; }).confirmed);
    CHECK(f.Apply(On(5)).submitted);
    CHECK(f.Apply(f.reflex, Reflex()).submitted);
}

void PartialInvalidationCannotCertifyInflightTransactions()
{
    for (bool submitReflex : {false, true})
    {
        Fixture f;
        CHECK(f.Apply(On()).confirmed);
        CHECK(f.Apply(f.reflex, Reflex()).confirmed);
        f.callback = [&] { f.state.InvalidateDlssg(); };
        const auto outer = submitReflex ? f.Apply(f.reflex, Reflex(), true) : f.Apply(On(), true);
        CHECK(outer.submitted && !outer.confirmed && outer.sdkResult == sl::Result::eOk);
        CHECK(outer.Result() == sl::Result::eErrorInvalidState && !f.state.InFlight());
        f.callback = {};
        CHECK(f.Apply(On()).submitted);
        CHECK(f.Apply(f.reflex, Reflex()).submitted);
    }
}

void FailuresAfterPartialInvalidationRevokeBothAcknowledgements()
{
    for (bool submitReflex : {false, true})
    {
        for (auto result : {sl::Result::eWarnOutOfVRAM, sl::Result::eErrorInvalidState})
        {
            Fixture f;
            CHECK(f.Apply(On()).confirmed);
            CHECK(f.Apply(f.reflex, Reflex()).confirmed);
            f.state.InvalidateDlssg();
            f.nextResult = result;
            const auto failed = submitReflex ? f.Apply(f.reflex, Reflex(), true) : f.Apply(On());
            CHECK(failed.submitted && !failed.confirmed && failed.sdkResult == result);
            f.nextResult = sl::Result::eOk;
            CHECK(f.Apply(On()).submitted);
            CHECK(f.Apply(f.reflex, Reflex()).submitted);
        }
    }
}

void SubmissionReasonsDescribeCallsAndHits()
{
    static_assert(std::string_view(ReasonName(Reason::AckHit)) == "AckHit");
    static_assert(std::string_view(ReasonName(Reason::Keepalive)) == "Keepalive");
    Fixture f;
    CHECK(f.Apply(On()).reason == Reason::Unacknowledged);
    CHECK(f.Apply(On()).reason == Reason::AckHit);
    CHECK(f.Apply(On(), true).reason == Reason::Forced);
    f.state.InvalidateDlssg();
    CHECK(f.Apply(On()).reason == Reason::Invalidated);
    ++f.context.provider.lifecycle;
    CHECK(f.Apply(On()).reason == Reason::ScopeChanged);
    CHECK(f.Apply(On(5)).reason == Reason::PayloadChanged);
    f.frame = 601;
    CHECK(f.Apply(On(5)).reason == Reason::Keepalive);
    f.frame = 600;
    CHECK(f.Apply(On(5)).reason == Reason::SourceRewind);
    f.eligible = false;
    CHECK(f.Apply(On(5)).reason == Reason::NotCacheable);
    f.current = false;
    const auto refused = f.Apply(On(5));
    CHECK(!refused.submitted && !refused.confirmed && refused.reason == Reason::NotCurrent);
    f.current = true;
    f.callback = [&] {
        const auto nested = f.Apply(f.reflex, Reflex());
        CHECK(!nested.submitted && !nested.confirmed && nested.reason == Reason::Reentrant);
    };
    CHECK(!f.Apply(On()).confirmed);
}

void OffFailureAndLifecycleRevokeBothAcknowledgements()
{
    Fixture f;
    CHECK(f.Apply(On()).confirmed);
    CHECK(f.Apply(f.reflex, Reflex()).confirmed);
    f.state.Invalidate();
    CHECK(f.Apply(On()).submitted);
    CHECK(f.Apply(f.reflex, Reflex()).submitted);
    auto off = On();
    off.mode = sl::DLSSGMode::eOff;
    CHECK(f.Apply(off).submitted);
    CHECK(f.Apply(off).submitted); // Teardown/Off must always reach the SDK.
    CHECK(f.Apply(f.reflex, Reflex()).submitted);
    CHECK(f.Apply(On()).submitted);
    f.nextResult = sl::Result::eErrorInvalidState;
    CHECK(!f.Apply(f.reflex, Reflex(), true).confirmed);
    f.nextResult = sl::Result::eOk;
    CHECK(f.Apply(On()).submitted); // A failed sibling API revokes the FG hit.
}

void ReleaseSharesDispatchTransactionAndNeverCaches()
{
    Fixture f;
    auto off = On(5);
    off.mode = sl::DLSSGMode::eOff;
    off.flags |= sl::DLSSGFlags::eRetainResourcesWhenOff;
    CHECK(f.Apply(On(5)).confirmed);
    CHECK(f.Apply(f.reflex, Reflex()).confirmed);
    unsigned releases = 0;
    const auto release = [&] {
        return f.state.Release(off, [&] { return f.current; }, [&] {
            ++releases;
            return f.nextResult;
        });
    };
    // Logical/source admission is already closed when teardown runs. Neither
    // the old running cache nor its source frame can suppress a real Off.
    f.eligible = false;
    f.frame = 0;
    CHECK(release().confirmed);
    CHECK(release().submitted && releases == 2);
    f.eligible = true;
    f.frame = 2;
    CHECK(f.Apply(On(5)).submitted);
    CHECK(f.Apply(f.reflex, Reflex()).submitted);
    f.nextResult = sl::Result::eWarnOutOfVRAM;
    CHECK(!release().confirmed);
    f.nextResult = sl::Result::eOk;
    CHECK(release().confirmed);
    CHECK(f.Apply(On(5)).submitted);
    f.current = false;
    const auto refused = release();
    CHECK(!refused.submitted && !refused.confirmed && releases == 4);
    f.current = true;
    CHECK(!f.state.Release(On(), [] { return true; }, [&] {
        ++releases; return sl::Result::eOk;
    }).submitted); // Release cannot accidentally enable generation.
    CHECK(releases == 4);
}

void ReleaseCallbacksCannotNestSdkCallsOrCertifyOldOwners()
{
    Fixture f;
    auto off = On();
    off.mode = sl::DLSSGMode::eOff;
    sl::ReflexOptions reflexOff {};
    unsigned releases = 0;
    // Dispatch -> Deactivate -> Off must not enter the SDK on its own callback.
    f.callback = [&] {
        const auto nested = f.state.Release(off, [] { return true; }, [&] {
            ++releases; return sl::Result::eOk;
        });
        CHECK(!nested.submitted && !nested.confirmed);
    };
    CHECK(f.Apply(On()).confirmed && releases == 0); // No SDK call or identity change occurred.
    f.callback = {};
    // Off -> recursive Off and Off -> recursive On share that same guard.
    const auto recursive = f.state.Release(off, [] { return true; }, [&] {
        ++releases;
        CHECK(!f.state.Release(reflexOff, [] { return true; }, [&] {
            ++releases; return sl::Result::eOk;
        }).submitted);
        CHECK(!f.Apply(On()).submitted);
        return sl::Result::eOk;
    });
    CHECK(recursive.submitted && !recursive.confirmed && releases == 1);
    CHECK(!f.state.InFlight());
    const auto owner = f.context.provider;
    const auto replaced = f.state.Release(reflexOff, [&] { return owner == f.context.provider; }, [&] {
        ++f.context.provider.lifecycle;
        return sl::Result::eOk;
    });
    CHECK(replaced.submitted && !replaced.confirmed);
    bool threw = false;
    try {
        f.state.Release(off, [] { return true; }, []() -> sl::Result {
            throw std::runtime_error("synthetic release failure");
        });
    } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw && !f.state.InFlight());
    CHECK(f.state.Release(reflexOff, [] { return true; }, [] { return sl::Result::eOk; }).confirmed);
    CHECK(f.Apply(On()).submitted);
}

// Uses the production Deactivate admission and release transactions together,
// not a second implementation of their serial/epoch/convergence rules. Static
// adapter checks below bind those same operations to DLSSG_Dx12's call sites.
void DeactivationAdapterConvergesWithRepeatedCallbacks()
{
    struct Adapter
    {
        ConfigurationState state;
        bool active=true, requested=false, fgPending=false, reflexPending=false;
        uint64_t inputEpoch=1, api=1;
        unsigned fgCalls=0, reflexCalls=0, retired=0;
        sl::Result reflexResult=sl::Result::eOk;
        std::function<void()> callback;
        void Deactivate()
        {
            if (!state.Deactivate(active, requested, [&] { state.Invalidate(); ++inputEpoch; })) return;
            active=false; fgPending=reflexPending=true;
            Boundary();
        }
        void Boundary()
        {
            if (state.InFlight()) return;
            const auto release=[&](auto options, bool& pending, unsigned& calls, sl::Result result) {
                if (!pending) return;
                const auto epoch=inputEpoch, entry=api;
                const auto outcome=state.Release(options, [&] { return epoch==inputEpoch && entry==api; }, [&] {
                    ++calls;
                    Deactivate(); // Vendor repeats this callback on EVERY attempt.
                    if(callback)callback();
                    return result;
                });
                if(outcome.confirmed)pending=false;
            };
            auto off=On(); off.mode=sl::DLSSGMode::eOff;
            release(off,fgPending,fgCalls,sl::Result::eOk);
            release(sl::ReflexOptions{},reflexPending,reflexCalls,reflexResult);
            if(!fgPending && !reflexPending && !retired)++retired; // CPU lifecycle only, no GPU claim.
        }
    } a;
    a.Deactivate();
    CHECK(a.inputEpoch==2 && a.fgCalls==1 && a.reflexCalls==1 && a.retired==1);
    for(unsigned i=0;i<20;++i){a.Deactivate();a.Boundary();}
    CHECK(a.inputEpoch==2 && a.fgCalls==1 && a.reflexCalls==1 && a.retired==1);
    Adapter failed;
    failed.reflexResult=sl::Result::eErrorInvalidState;
    failed.Deactivate();
    CHECK(!failed.fgPending && failed.reflexPending && failed.retired==0);
    failed.reflexResult=sl::Result::eOk; failed.Boundary();
    CHECK(!failed.reflexPending && failed.fgCalls==1 && failed.reflexCalls==2 && failed.retired==1);
    Adapter changed;
    changed.callback=[&]{++changed.api;};
    changed.Deactivate();
    CHECK(changed.fgPending && changed.reflexPending && changed.retired==0);
    changed.callback={}; changed.Boundary();
    CHECK(!changed.fgPending && !changed.reflexPending && changed.retired==1);
    Adapter generation;
    generation.callback=[&]{++generation.inputEpoch;};
    generation.Deactivate();
    CHECK(generation.fgPending && generation.reflexPending && generation.retired==0);
    generation.callback={}; generation.Boundary();
    CHECK(generation.retired==1);
    Adapter exception;
    exception.callback=[] {throw std::runtime_error("FG Off interrupted");};
    bool threw=false;
    try {exception.Deactivate();}catch(const std::runtime_error&){threw=true;}
    CHECK(threw && !exception.active && exception.fgPending && exception.reflexPending);
    CHECK(!exception.state.InFlight() && exception.reflexCalls==0);
    exception.callback={};
    exception.Deactivate(); // Idempotence must not erase either registered intent.
    exception.Boundary();
    CHECK(exception.fgCalls==2 && exception.reflexCalls==1 && exception.retired==1);
    CHECK(!exception.fgPending && !exception.reflexPending);
}

void EveryOwnerAndConfigurationBoundaryMisses()
{
    using Mutate = void (*)(Scope&);
    const Mutate mutations[] = {
        [](Scope& s) { ++s.provider.instance; }, [](Scope& s) { ++s.provider.lifecycle; },
        [](Scope& s) { ++s.provider.device; }, [](Scope& s) { ++s.provider.queue; },
        [](Scope& s) { ++s.provider.swapchain; }, [](Scope& s) { ++s.provider.api; },
        [](Scope& s) { ++s.provider.plugin; }, [](Scope& s) { ++s.provider.setOptions; },
        [](Scope& s) { ++s.provider.getState; }, [](Scope& s) { ++s.provider.viewport; },
        [](Scope& s) { ++s.provider.width; }, [](Scope& s) { ++s.provider.height; },
        [](Scope& s) { ++s.input.epoch; }, [](Scope& s) { ++s.sourceGeneration; },
        [](Scope& s) { ++s.input.frames; }, [](Scope& s) { ++s.input.dynamicMode; },
        [](Scope& s) { ++s.input.dynamicTarget; }, [](Scope& s) { ++s.input.depthScaleBits; },
        [](Scope& s) { ++s.input.flipOffset; }, [](Scope& s) { ++s.reflexPlugin; },
        [](Scope& s) { ++s.reflexSetOptions; }, [](Scope& s) { ++s.inputPath; },
        [](Scope& s) { ++s.outputPath; }, [](Scope& s) { ++s.interop; },
        [](Scope& s) { ++s.providerCap; },
        [](Scope& s) { ++s.consumers; }, [](Scope& s) { ++s.forceReflex; },
        [](Scope& s) { ++s.guideFlags; }, [](Scope& s) { ++s.colorSpace; },
        [](Scope& s) { ++s.transfer; }, [](Scope& s) { ++s.primaries; },
        [](Scope& s) { ++s.range; }, [](Scope& s) { ++s.colorModel; },
        [](Scope& s) { s.colorSpaceValid = !s.colorSpaceValid; },
        [](Scope& s) { s.enabled = !s.enabled; }, [](Scope& s) { s.boost = !s.boost; },
        [](Scope& s) { s.markers = !s.markers; }, [](Scope& s) { s.sharedSdk = !s.sharedSdk; },
        [](Scope& s) { s.external = !s.external; }, [](Scope& s) { s.secondDevice = !s.secondDevice; }
    };
    for (auto mutate : mutations)
    {
        Fixture f;
        CHECK(f.Apply(On()).confirmed);
        CHECK(f.Apply(f.reflex, Reflex()).confirmed);
        f.state.InvalidateDlssg(); // Retaining Reflex never bypasses its complete scope validation.
        mutate(f.context);
        CHECK(f.Apply(On()).submitted);
        CHECK(f.Apply(f.reflex, Reflex()).reason == Reason::ScopeChanged);
        CHECK(!f.Apply(On()).submitted);
        CHECK(!f.Apply(f.reflex, Reflex()).submitted);
        f.context = Context();
        CHECK(f.Apply(On()).submitted); // Leaving then returning cannot use the earlier acknowledgement.
        CHECK(f.Apply(f.reflex, Reflex()).submitted);
    }
    Fixture separateOwner;
    CHECK(separateOwner.Apply(On()).submitted);
}

void PayloadChangesReachTheSdk()
{
    using Mutate = void (*)(sl::DLSSGOptions&);
    const Mutate changes[] = {
        [](auto& o) { o.numFramesToGenerate = 5; },
        [](auto& o) { o.mode = sl::DLSSGMode::eDynamic; },
        [](auto& o) { o.flags |= sl::DLSSGFlags::eRetainResourcesWhenOff; },
        [](auto& o) { o.dynamicResWidth = 100; }, [](auto& o) { o.dynamicResHeight = 100; },
        [](auto& o) { o.numBackBuffers = 3; }, [](auto& o) { o.mvecDepthWidth = 100; },
        [](auto& o) { o.mvecDepthHeight = 100; }, [](auto& o) { o.colorWidth = 100; },
        [](auto& o) { o.colorHeight = 100; }, [](auto& o) { o.colorBufferFormat = 28; },
        [](auto& o) { o.mvecBufferFormat = 16; }, [](auto& o) { o.depthBufferFormat = 40; },
        [](auto& o) { o.hudLessBufferFormat = 28; }, [](auto& o) { o.uiBufferFormat = 28; },
        [](auto& o) { o.bReserved15 = sl::eTrue; },
        [](auto& o) { o.enableUserInterfaceRecomposition = sl::eTrue; },
        [](auto& o) { o.dynamicTargetFrameRate = 120.0f; }
    };
    for (auto mutate : changes)
    {
        Fixture f;
        CHECK(f.Apply(On()).confirmed);
        auto changed = On();
        mutate(changed);
        CHECK(f.Apply(changed).submitted);
        CHECK(!f.Apply(changed).submitted);
    }
    using ReflexMutate = void (*)(sl::ReflexOptions&);
    const ReflexMutate reflexChanges[] = {
        [](auto& o) { o.mode = sl::ReflexMode::eLowLatencyWithBoost; },
        [](auto& o) { o.frameLimitUs = 16667; }, [](auto& o) { o.useMarkersToOptimize = true; },
        [](auto& o) { o.virtualKey = 0x7C; }, [](auto& o) { o.idThread = 123; }
    };
    for (auto mutate : reflexChanges)
    {
        Fixture f;
        CHECK(f.Apply(f.reflex, Reflex()).confirmed);
        auto changed = Reflex();
        mutate(changed);
        CHECK(f.Apply(f.reflex, changed).submitted);
        CHECK(!f.Apply(f.reflex, changed).submitted);
    }
}

void ErrorCallback(const sl::APIError&) {}

void UnknownSchemasCallbacksAndUnownedRoutesNeverCache()
{
    sl::DLSSGOptions extension {};
    using Mutate = void (*)(sl::DLSSGOptions&);
    const Mutate changes[] = {
        [](auto& o) { o.structVersion = sl::kStructVersion4; },
        [](auto& o) { o.structVersion = 99; }, [](auto& o) { o.structType.data1 ^= 1; },
        [](auto& o) { o.onErrorCallback = &ErrorCallback; },
        [](auto& o) { o.flags = static_cast<sl::DLSSGFlags>(0x80000000u); },
        [](auto& o) { o.mode = sl::DLSSGMode::eCount; },
        [](auto& o) { o.queueParallelismMode = sl::DLSSGQueueParallelismMode::eBlockNoClientQueues; },
        [](auto& o) { o.enableUserInterfaceRecomposition = sl::eInvalid; },
        [](auto& o) { o.numFramesToGenerate = 0; }, [](auto& o) { o.numFramesToGenerate = 6; },
        [](auto& o) { o.dynamicTargetFrameRate = (std::numeric_limits<float>::infinity)(); },
        [](auto& o) { o.dynamicTargetFrameRate = (std::numeric_limits<float>::quiet_NaN)(); }
    };
    for (auto mutate : changes)
    {
        Fixture f;
        auto unknown = On();
        mutate(unknown);
        CHECK(f.Apply(On()).confirmed);
        CHECK(f.Apply(f.reflex, Reflex()).confirmed);
        f.state.InvalidateDlssg();
        const auto first = f.Apply(unknown);
        CHECK(first.submitted && !first.confirmed && first.sdkResult == sl::Result::eOk);
        CHECK(first.Result() == sl::Result::eErrorInvalidState);
        CHECK(f.Apply(unknown).submitted);
        CHECK(f.Apply(On()).submitted);
        CHECK(f.Apply(f.reflex, Reflex()).submitted);
    }
    Fixture chained;
    auto options = On();
    options.next = &extension;
    CHECK(!chained.Apply(options).confirmed);
    CHECK(chained.Apply(options).submitted);
    Fixture unowned;
    unowned.eligible = false;
    CHECK(unowned.Apply(On()).submitted);
    CHECK(unowned.Apply(On()).submitted);
    auto unknownReflex = Reflex();
    unknownReflex.next = &extension;
    CHECK(!unowned.Apply(unowned.reflex, unknownReflex).confirmed);
    CHECK(unowned.Apply(unowned.reflex, unknownReflex).submitted);
    Fixture sibling;
    CHECK(sibling.Apply(On()).confirmed);
    CHECK(sibling.Apply(sibling.reflex, Reflex()).confirmed);
    sibling.state.InvalidateDlssg();
    CHECK(!sibling.Apply(sibling.reflex, unknownReflex).confirmed);
    CHECK(sibling.Apply(On()).submitted);
    CHECK(sibling.Apply(sibling.reflex, Reflex()).submitted);
}

void RecursiveAndChangedOwnerResultsFailClosed()
{
    Fixture f;
    CHECK(f.Apply(On()).confirmed);
    CHECK(f.Apply(f.reflex, Reflex()).confirmed);
    f.state.InvalidateDlssg();
    f.callback = [&] {
        const auto nested = f.Apply(f.reflex, Reflex());
        CHECK(!nested.submitted && !nested.confirmed);
    };
    const auto outer = f.Apply(On());
    CHECK(outer.submitted && outer.sdkResult == sl::Result::eOk && !outer.confirmed);
    CHECK(outer.Result() == sl::Result::eErrorInvalidState && f.calls == 3);
    f.callback = {};
    CHECK(f.Apply(On()).submitted);
    CHECK(f.Apply(f.reflex, Reflex()).submitted);

    Fixture moved;
    moved.callback = [&] { ++moved.context.provider.lifecycle; };
    CHECK(!moved.Apply(On()).confirmed);
    moved.callback = {};
    CHECK(moved.Apply(On()).submitted);
    moved.callback = [&] { ++moved.frame; };
    CHECK(!moved.Apply(On(), true).confirmed);
    moved.callback = {};
    moved.current = false;
    CHECK(!moved.Apply(On()).submitted);
    moved.current = true;
    CHECK(moved.Apply(On()).submitted);
}

void ThrowingSubmissionsRevokeSiblingsAndRecover()
{
    for (bool submitReflex : {false, true})
    {
        Fixture f;
        CHECK(f.Apply(On()).confirmed);
        CHECK(f.Apply(f.reflex, Reflex()).confirmed);
        f.state.InvalidateDlssg();
        f.callback = [] { throw std::runtime_error("synthetic SDK failure"); };
        bool threw = false;
        try { static_cast<void>(submitReflex ? f.Apply(f.reflex, Reflex(), true) : f.Apply(On())); }
        catch (const std::runtime_error&) { threw = true; }
        CHECK(threw && !f.state.InFlight());
        f.callback = {};
        CHECK(f.Apply(f.reflex, Reflex()).submitted);
        CHECK(f.Apply(On()).submitted);
    }
}

void SourceRewindsCannotAgeOrReuseAnAcknowledgement()
{
    Fixture f;
    f.frame = 100;
    CHECK(f.Apply(On()).confirmed);
    f.frame = 99;
    CHECK(f.Apply(On()).submitted);
    ++f.context.sourceGeneration;
    CHECK(f.Apply(On()).submitted);
    f.state.Invalidate(); // Includes real hold/off/release/resize boundaries.
    CHECK(f.Apply(On()).submitted);
}

void RawCapabilityBoundsRequestedAndAcceptedConfiguration()
{
    GenerationState state;
    const auto scope = Context();
    CHECK(!state.Select(scope.provider, 5, false));
    sl::DLSSGState raw {};
    raw.numFramesToGenerateMax = 1;
    raw.numFramesActuallyPresented = 6; // A query count cannot raise capability.
    state.Observe(scope.provider, raw, sl::Result::eOk, true);
    const auto bounded = state.Select(scope.provider, 5, false);
    CHECK(bounded && bounded.requestedGenerated == 5 && bounded.attemptedGenerated == 1);
    state.Record(scope, 42, bounded, On(1), { sl::Result::eOk, true, true });
    CHECK(state.requestedGenerated == 5 && state.acceptedGenerated == 1 && state.FixedTotal() == 2);
    CHECK(state.Accepted(scope, 42) && !state.Accepted(scope, 43));
    CHECK(state.rawMaxGenerated == 1 && state.rawPresented == 6);
    state.Record(scope, 42, bounded, On(5), { sl::Result::eOk, true, true });
    CHECK(!state.acceptedKnown && state.FixedTotal() == 0); // Synthetic success above raw max is not authority.

    raw.numFramesToGenerateMax = 5;
    raw.numFramesActuallyPresented = 0;
    state.Observe(scope.provider, raw, sl::Result::eOk, true);
    const auto six = state.Select(scope.provider, 5, false);
    state.Record(scope, 43, six, On(5), { sl::Result::eOk, true, true });
    CHECK(state.FixedTotal() == 6 && state.rawPresented == 0);
    raw.numFramesToGenerateMax = 1;
    state.Observe(scope.provider, raw, sl::Result::eOk, true);
    CHECK(!state.acceptedKnown && state.FixedTotal() == 0);
    raw.numFramesToGenerateMax = 5;
    state.Observe(scope.provider, raw, sl::Result::eOk, true);
    auto otherOwner = scope.provider;
    ++otherOwner.lifecycle;
    CHECK(!state.Select(otherOwner, 5, false));
    auto otherSource = scope;
    ++otherSource.sourceGeneration;
    CHECK(!state.Accepted(otherSource, 43));
    ++otherSource.input.epoch;
    CHECK(!state.Accepted(otherSource, 43));

    state.Record(scope, 44, six, On(5), { sl::Result::eWarnOutOfVRAM, true, false });
    CHECK(!state.acceptedKnown && state.FixedTotal() == 0);
    state.Record(scope, 44, six, On(5), { sl::Result::eOk, true, false });
    CHECK(!state.acceptedKnown); // Reentered eOk is not an applied receipt.
    auto off = On(5);
    off.mode = sl::DLSSGMode::eOff;
    state.Record(scope, 44, six, off, { sl::Result::eOk, true, true });
    CHECK(state.acceptedKnown && state.acceptedGenerated == 0 && state.FixedTotal() == 0);
}

void CapFailuresSchemasAndDynamicModeRemainIndependent()
{
    GenerationState state;
    const auto scope = Context();
    sl::DLSSGState raw {};
    raw.numFramesToGenerateMax = 5;
    raw.bIsDynamicMFGSupported = sl::eFalse;
    state.Observe(scope.provider, raw, sl::Result::eOk, true);
    CHECK(!state.Select(scope.provider, 5, true).dynamic); // Fixed 6x does not imply dynamic support.
    raw.bIsDynamicMFGSupported = sl::eTrue;
    state.Observe(scope.provider, raw, sl::Result::eOk, true);
    const auto dynamic = state.Select(scope.provider, 5, true);
    auto options = On(5);
    options.mode = sl::DLSSGMode::eDynamic;
    state.Record(scope, 1, dynamic, options, { sl::Result::eOk, true, true });
    CHECK(dynamic.dynamic && state.acceptedKnown && state.acceptedGenerated == 5 && state.FixedTotal() == 0);
    raw.bIsDynamicMFGSupported = sl::eInvalid;
    state.Observe(scope.provider, raw, sl::Result::eOk, true);
    CHECK(!state.Select(scope.provider, 5, true).dynamic);
    raw.numFramesToGenerateMax = UINT32_MAX;
    state.Observe(scope.provider, raw, sl::Result::eOk, true);
    CHECK(state.rawMaxGenerated == UINT32_MAX && state.Maximum(scope.provider) == 5);
    raw.numFramesToGenerateMax = 0;
    state.Observe(scope.provider, raw, sl::Result::eOk, true);
    CHECK(!state.Select(scope.provider, 5, false));
    raw.numFramesToGenerateMax = 5;
    state.Observe(scope.provider, raw, sl::Result::eWarnOutOfVRAM, true);
    CHECK(!state.capabilityKnown && !state.Select(scope.provider, 5, false));
    state.Observe(scope.provider, raw, sl::Result::eOk, false);
    CHECK(!state.capabilityKnown);
    raw.structVersion = 3;
    state.Observe(scope.provider, raw, sl::Result::eOk, true);
    CHECK(!state.capabilityKnown);
    raw.structVersion = 4;
    raw.structType.data1 ^= 1;
    state.Observe(scope.provider, raw, sl::Result::eOk, true);
    CHECK(!state.capabilityKnown);
    raw = sl::DLSSGState{};
    raw.numFramesToGenerateMax = 5;
    sl::DLSSGOptions extension {};
    raw.next = &extension;
    state.Observe(scope.provider, raw, sl::Result::eOk, true);
    CHECK(!state.capabilityKnown);
    raw.next = nullptr;
    state.Observe(scope.provider, raw, sl::Result::eOk, true);
    state.InvalidateCapability();
    CHECK(!state.capabilityKnown && !state.acceptedKnown && !state.Select(scope.provider, 5, false));
}

void UserInterfaceRecompositionRequiresAcceptedCurrentPair()
{
    CHECK(!CanRecomposeUserInterface(false, false, true));
    CHECK(!CanRecomposeUserInterface(true, false, true));
    CHECK(!CanRecomposeUserInterface(false, true, true));
    CHECK(!CanRecomposeUserInterface(true, true, false));
    CHECK(CanRecomposeUserInterface(true, true, true));
}

void ProviderReflexUsesQualifiedSdkDefaults()
{
    for (bool boost : {false, true})
    {
        const auto options = ProviderReflexOptions(boost);
        CHECK(options.mode == (boost ? sl::ReflexMode::eLowLatencyWithBoost : sl::ReflexMode::eLowLatency));
        CHECK(!options.useMarkersToOptimize);
        CHECK(options.frameLimitUs == 0 && options.virtualKey == 0 && options.idThread == 0);
        CHECK(Known(options));
        Fixture f;
        CHECK(f.Apply(f.reflex, options).confirmed);
        CHECK(!f.Apply(f.reflex, options).submitted);
        auto old = options;
        old.useMarkersToOptimize = true;
        CHECK(f.Apply(f.reflex, old).submitted);
        CHECK(f.Apply(f.reflex, options).submitted);
    }
}
}

int main()
{
    SuccessfulAcknowledgementIsReusedAcrossSlots();
    UnownedSdkRequiresARealCallEverySource();
    SixfoldSourcesReuseOptionsButFreshGapRecoveryReallyReenables();
    KeepaliveUsesSourceAgeAndRetriesWarnings();
    RetainedFgGapsPreserveReflexScopeAndKeepalive();
    PartialInvalidationCannotCertifyInflightTransactions();
    FailuresAfterPartialInvalidationRevokeBothAcknowledgements();
    SubmissionReasonsDescribeCallsAndHits();
    OffFailureAndLifecycleRevokeBothAcknowledgements();
    ReleaseSharesDispatchTransactionAndNeverCaches();
    ReleaseCallbacksCannotNestSdkCallsOrCertifyOldOwners();
    DeactivationAdapterConvergesWithRepeatedCallbacks();
    EveryOwnerAndConfigurationBoundaryMisses();
    PayloadChangesReachTheSdk();
    UnknownSchemasCallbacksAndUnownedRoutesNeverCache();
    RecursiveAndChangedOwnerResultsFailClosed();
    ThrowingSubmissionsRevokeSiblingsAndRecover();
    SourceRewindsCannotAgeOrReuseAnAcknowledgement();
    RawCapabilityBoundsRequestedAndAcceptedConfiguration();
    CapFailuresSchemasAndDynamicModeRemainIndependent();
    UserInterfaceRecompositionRequiresAcceptedCurrentPair();
    ProviderReflexUsesQualifiedSdkDefaults();
    std::printf("DlssgOptionsPolicy CPU contracts: %u checks passed\n", checks);
}
