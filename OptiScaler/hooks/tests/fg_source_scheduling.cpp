// CPU contracts only. The runner inserts the REAL FGExecutionGuard and
// PresentMutexRelease blocks; no graphics device, SDK DLL or game is started.
#include <framegen/DlssgStateSample.h>
#include <framegen/FgSourcePacing.h>
#include <framegen/FrameLatencyOwnership.h>
#include <framegen/FgPacingLease.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

// INSERT_EXECUTION_GUARD

namespace
{
using namespace std::chrono_literals;
unsigned checks=0;
void Check(bool ok,const char* message)
{
    ++checks;
    if(!ok)throw std::runtime_error(message);
}
struct OwnedMutex
{
    std::mutex mutex;
    bool locked=false;
    void lock(unsigned) {mutex.lock();locked=true;}
    void unlockThis(unsigned) {locked=false;mutex.unlock();}
};
// INSERT_MUTEX_RELEASE

bool OtherThreadCanLock(std::recursive_mutex& mutex)
{
    bool acquired=false;
    std::thread worker([&]{acquired=mutex.try_lock();if(acquired)mutex.unlock();});
    worker.join();return acquired;
}
struct Token final : sl::FrameToken
{
    uint32_t frame;
    explicit Token(uint32_t value):frame(value){}
    operator uint32_t() const override {return frame;}
};
struct SdkLifetime
{
    Token original{123};
    unsigned sleepCalls=0;
    std::atomic<bool> providerGone{false};
};
class TestLease final : public FGPacingLease
{
    std::shared_ptr<SdkLifetime> sdk_;
    sl::FrameToken* original_;
public:
    explicit TestLease(std::shared_ptr<SdkLifetime> sdk):sdk_(std::move(sdk)),original_(&sdk_->original){}
    bool Valid() const noexcept override {return original_==&sdk_->original && static_cast<uint32_t>(*original_)==123;}
    uint64_t Frame() const noexcept override {return static_cast<uint32_t>(*original_);}
    sl::Result Sleep() noexcept override {
        if(!Valid() || !sdk_->providerGone.load())return sl::Result::eErrorInvalidState;
        ++sdk_->sleepCalls;return sl::Result::eOk;
    }
};
struct Provider
{
    std::recursive_mutex continuity;
    OwnedMutex fg;
    unsigned workDepth=0;
    std::shared_ptr<SdkLifetime> sdk=std::make_shared<SdkLifetime>();
    ~Provider(){sdk->providerGone.store(true);}
};

void PacingAndTeardown()
{
    auto provider=std::make_unique<Provider>();
    auto sdk=provider->sdk;
    provider->fg.lock(2);
    FGExecutionGuard execution(provider->continuity,&provider->workDepth);
    PresentMutexRelease release{&provider->fg};
    Check(provider->workDepth==1,"real guard enters provider work");
    execution.FinishWork();
    Check(provider->workDepth==0,"FinishWork retires work depth");
    Check(!OtherThreadCanLock(provider->continuity),"FinishWork DOES NOT release continuity");
    FGPacingLeasePtr lease=std::make_shared<TestLease>(sdk);
    auto input=framegen::SourcePacingInput{framegen::PacingRole::Source,true,true,true,true,true,true,
        false,false,false,true,false,5};
    const auto decision=framegen::SelectSourcePacing(input);
    input.fgActive=false;input.generatedFrames=0; // mutation cannot rewrite this transaction's decision
    Check(decision.reflexSleep && decision.frameLimit && decision.generatedFrames==5,
        "latency Reflex and explicit user cap remain legal together");
    release.Release();
#ifdef F6_NEGATIVE_LOCKED_PACING
    execution.FinishWork(); // Exact old behavior: zero depth but still owns the lock.
#else
    execution.Finish();
#endif
    Check(OtherThreadCanLock(provider->continuity),"pacing starts only after execution/continuity unlock");
    std::thread teardown([&] {
        auto* owned=provider.get();
        {std::lock_guard fg(owned->fg.mutex);std::lock_guard continuity(owned->continuity);}
        provider.reset();
    });
    teardown.join();
    Check(sdk->providerGone.load() && lease->Valid(),"independent SDK lease survives provider destruction");
    Check(lease->Sleep()==sl::Result::eOk && sdk->sleepCalls==1,
        "post-unlock Sleep uses exactly the original retained SDK token once");
    lease.reset();
    Check(sdk.use_count()==1,"destroying the lease releases its CPU/SDK owner");
}

void SourcePolicy()
{
    using namespace framegen;
    const SourcePacingInput base{PacingRole::Source,true,true,true,true,true,true,
        false,false,false,true,false,5};
    for(unsigned cap=0;cap<8;++cap){
        auto in=base;in.reflexOwnsCap=(cap&1)!=0;in.nativeOwnsCap=(cap&2)!=0;in.xellOwnsCap=(cap&4)!=0;
        const auto decision=SelectSourcePacing(in);
        Check(decision.reflexSleep,"cap ownership does not disable latency Reflex");
        Check(decision.frameLimit==(cap==0),"another cap owner excludes only the extra cap wait");
    }
    for(unsigned changed=0;changed<7;++changed){
        auto in=base;
        switch(changed){
        case 0:in.role=PacingRole::GeneratedOutput;break;
        case 1:in.role=PacingRole::Unknown;break;
        case 2:in.realPresent=false;break;
        case 3:in.directDlss=false;break;
        case 4:in.ownedDlssgProvider=false;break;
        case 5:in.tokenLeaseValid=false;break;
        case 6:in.ownMarkers=false;break;
        }
        const auto decision=SelectSourcePacing(in);
        Check(!decision.reflexSleep,"generated/unknown/TEST/foreign/invalid-token/game-markers cannot sleep source Reflex");
        if(changed<5)Check(!decision.frameLimit,"generated/TEST/foreign callbacks cannot repeat the source cap");
    }
    auto off=base;off.fgActive=false;
    const auto offDecision=SelectSourcePacing(off);
    Check(!offDecision.reflexSleep && offDecision.frameLimit && offDecision.generatedFrames==0,
        "FG Off keeps the independent explicit source cap without an FG multiplier");
    SourcePacingScope source(true);
    SourcePacingScope callback(true);
    SourcePacingScope physical(false);
    Check(source.Owns() && !callback.Owns() && !physical.Owns(),"one source owns policy across synchronous callbacks");
    bool workerOwns=true;
    std::thread worker([&]{SourcePacingScope generated(false);workerOwns=generated.Owns();});worker.join();
    Check(!workerOwns,"independent SDK output worker gets no source policy authority");
}

void CounterIntervals()
{
    using namespace framegen;
    DlssgStateQueries queries;
    unsigned calls=0,pending=7;
    auto sdk=[&](DlssgStateSample& sample){++calls;sample.succeeded=true;sample.result=0;
        sample.actuallyPresented=std::exchange(pending,0u);sample.maxGenerated=5;sample.stateVersion=4;};
    DlssgStateSample first,second,cached;
    Check(queries.Query(0x10,2,DlssgStateQueryReason::CompletionFence,first,sdk),"necessary fence query runs once");
    Check(calls==1 && first.querySequence==1 && first.actuallyPresented==7 && !first.counterIntervalKnown,
        "cold query preserves SDK interval count rather than requested multiplier+1");
    Check(first.queryEndNs>=first.queryBeginNs && first.queryBeginNs!=0,"sample has real monotonic ns endpoints");
    pending=3;
    for(unsigned i=0;i<1000;++i)
        Check(queries.Read(0x10,2,cached) && cached.maxGenerated==5,"capability/menu cache reads existing state");
    Check(calls==1 && pending==3,"cold capability reuse and menu reads do not consume SDK counts");
    Check(queries.Query(0x10,2,DlssgStateQueryReason::Provider,second,sdk),"next real query runs");
    Check(second.actuallyPresented==3 && second.previousQuerySequence==first.querySequence &&
        second.previousQueryEndNs==first.queryEndNs && second.counterIntervalKnown,
        "counter interval is bound to its actual predecessor sequence/time");
    Check(queries.SubmitOptions([&]{pending=0;}),"serialized options retain their API call");
    Check(queries.Query(0x10,2,DlssgStateQueryReason::Provider,second,sdk) && second.actuallyPresented==0 &&
        !second.counterIntervalKnown,"options/counter reset does not fabricate an uninterrupted interval");
    queries.Query(0x10,2,DlssgStateQueryReason::Other,second,[](auto& value){value.result=-2;});
    Check(!second.succeeded && !second.counterIntervalKnown,"failed state is explicit unknown");
    queries.Query(0x10,2,DlssgStateQueryReason::Provider,second,sdk);
    Check(!second.counterIntervalKnown,"failure cannot stretch the next counter interval back to older success");
    queries.Query(0x10,3,DlssgStateQueryReason::Provider,second,sdk);
    Check(!second.counterIntervalKnown && second.previousViewport==2,"changed viewport interval remains explicit unknown");
    bool recursiveAccepted=true,optionsAccepted=true,readInside=false;
    queries.Query(0x10,3,DlssgStateQueryReason::Provider,second,[&](auto& value){
        DlssgStateSample nested;
        readInside=queries.Read(0x10,3,nested);
        recursiveAccepted=queries.Query(0x10,3,DlssgStateQueryReason::Other,nested,sdk);
        optionsAccepted=queries.SubmitOptions([]{});
        value.succeeded=true;
    });
    Check(readInside && !recursiveAccepted && !optionsAccepted,"SDK callback can read cache but cannot recursively query/mutate SDK");
    try{queries.Query(0x10,3,DlssgStateQueryReason::Provider,second,[](auto&){throw std::runtime_error("vendor");});}
    catch(const std::runtime_error&){}
    Check(queries.Read(0x10,3,cached) && !cached.succeeded,"throwing query publishes failed interval");
    queries.Query(0x10,3,DlssgStateQueryReason::Provider,second,sdk);
    Check(!second.counterIntervalKnown && second.previousQuerySequence==cached.querySequence,
        "unwind releases serialization and retains the failed predecessor");
    for(uint32_t viewport=100;viewport<140;++viewport)
        queries.Query(0x20,viewport,DlssgStateQueryReason::Other,second,sdk);
    Check(!queries.Read(0x10,2,cached),"bounded cache evicts old identities instead of growing without limit");
}

void ConcurrentQueriesAndOptions()
{
    using namespace framegen;
    DlssgStateQueries queries;
    std::atomic<unsigned> active{0},overlap{0},queryCalls{0},optionCalls{0},readerErrors{0};
    const auto enter=[&]{if(active.fetch_add(1)!=0)++overlap;std::this_thread::yield();};
    const auto leave=[&]{--active;};
    std::vector<std::thread> workers;
    for(unsigned thread=0;thread<6;++thread)workers.emplace_back([&,thread]{
        for(unsigned i=0;i<200;++i){
            if(thread%2){queries.SubmitOptions([&]{enter();++optionCalls;leave();});}
            else {DlssgStateSample sample;queries.Query(0x42,1,DlssgStateQueryReason::Provider,sample,[&](auto& value){
                enter();++queryCalls;value.succeeded=true;value.actuallyPresented=0;leave();});}
        }
    });
    workers.emplace_back([&]{for(unsigned i=0;i<2000;++i){DlssgStateSample sample;
        if(queries.Read(0x42,1,sample) && (sample.querySequence==0 || sample.queryEndNs<sample.queryBeginNs))++readerErrors;}});
    for(auto& worker:workers)worker.join();
    Check(queryCalls==600 && optionCalls==600,"all concurrent real query/options calls execute exactly once");
    Check(overlap==0 && active==0,"same SDK not-thread-safe query/options invocations never overlap");
    Check(readerErrors==0,"concurrent cached observations are complete snapshots");
}

void WaitableOwnership()
{
    using namespace framegen::latency;
    const Flags noRequest{0,0,WaitableFlag,true,true,true};
    const Flags requested{WaitableFlag,WaitableFlag,WaitableFlag,true,true,true};
    Check(Select(noRequest,Caller::Application,true,true,true,false)==Target::None,
        "app without requested WAITABLE cannot steal the loaded SDK object");
    // FG on/off is deliberately not an input to this ownership policy.
    Check(Select(noRequest,Caller::Application,true,true,true,true)==Target::None,
        "loaded plugin retains ownership even for native-only/FG-off route");
    Check(Select(requested,Caller::Application,true,true,true,false)==Target::Native,
        "explicit app WAITABLE request preserves its native object");
    auto providerOnly=requested;providerOnly.effectiveNative=0;
    Check(Select(providerOnly,Caller::Application,true,true,true,false)==Target::Provider,
        "explicit app request retains existing real provider object when native has none");
    for(auto role:{Caller::Sdk,Caller::Unknown})
        Check(Select(noRequest,role,true,true,true,false)==Target::Preserve,
            "SDK and unproven caller roles always pass through");
    Check(Select(noRequest,Caller::Application,false,true,true,false)==Target::Preserve,
        "without loaded SDK preserve application's ordinary DXGI behavior");
    auto unknown=noRequest;unknown.requestedKnown=false;
    Check(Select(unknown,Caller::Application,true,true,true,false)==Target::Preserve,
        "effective flags are never substituted for unknown original application flags");
}
}

int main() try
{
    PacingAndTeardown();SourcePolicy();CounterIntervals();ConcurrentQueriesAndOptions();WaitableOwnership();
    std::printf("PASS %u FG scheduling/query/waitable CPU checks (no SDK/game/GPU)\n",checks);
    return 0;
}
catch(const std::exception& error){std::fprintf(stderr,"FAIL: %s\n",error.what());return 1;}
