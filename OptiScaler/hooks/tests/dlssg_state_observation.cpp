// The runner inserts the production QueryDlssgState body. Real SDK headers and
// Windows ABI/LastError, scripted SDK calls; no game, graphics device or SDK DLL.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <sl_dlss_g.h>
#include <framegen/DlssgStateSample.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>

namespace
{
unsigned checks=0,calls=0,observations=0;
bool throwObserver=false;
framegen::DlssgStateSample lastObserved;
framegen::DlssgStateQueries g_dlssgStateQueries;
void Check(bool ok,const char* message)
{
    ++checks;
    if (!ok) throw std::runtime_error(message);
}
void ObserveDlssgState(const framegen::DlssgStateSample& sample,bool) noexcept
{
    ++observations;
    lastObserved=sample;
    // Like the production observer, a failed diagnostic remains contained.
    try { SetLastError(0xBAD1); if(throwObserver)throw std::runtime_error("observer"); }
    catch (...) {}
}
}

class StreamlineHooks
{
public:
    inline static decltype(&slDLSSGGetState) o_slDLSSGGetState=nullptr;
    static sl::Result QueryDlssgState(decltype(&slDLSSGGetState),const sl::ViewportHandle&,
        sl::DLSSGState&,const sl::DLSSGOptions*,framegen::DlssgStateQueryReason,
        framegen::DlssgStateSample* =nullptr);
    static sl::Result hkslDLSSGGetState(const sl::ViewportHandle& v,sl::DLSSGState& s,const sl::DLSSGOptions* o)
    {return QueryDlssgState(o_slDLSSGGetState,v,s,o,framegen::DlssgStateQueryReason::Game);}
    static sl::Result hkslDLSSGGetStateSerial(const sl::ViewportHandle& v,sl::DLSSGState& s,const sl::DLSSGOptions* o)
    {return QueryDlssgState(o_slDLSSGGetState,v,s,o,framegen::DlssgStateQueryReason::Other);}
};

// INSERT_PRODUCTION_QUERY

namespace
{
enum class Mode { Full, Failure, Throw, RewriteVersion, WrongType, Reenter };
Mode mode=Mode::Full;
uint32_t rewriteVersion=4;
constexpr DWORD sdkError=0xA123;
constexpr uintptr_t poisonFence=0xDEAD0010;
constexpr uint64_t target=0xFEDCBA9876543210ull;
unsigned recursiveCalls=0;
const sl::DLSSGOptions* expectedOptions=nullptr;

sl::Result Sdk(const sl::ViewportHandle&,sl::DLSSGState& state,const sl::DLSSGOptions* options)
{
    ++calls;
    Check(options==expectedOptions,"original options pointer reaches SDK unchanged");
    if(mode==Mode::Throw){SetLastError(sdkError);throw std::runtime_error("vendor sentinel");}
    if(mode==Mode::Failure){SetLastError(sdkError);return sl::Result::eErrorInvalidState;}
    if(mode==Mode::RewriteVersion) state.structVersion=rewriteVersion;
    else if(mode==Mode::WrongType) state.structType.data1^=1;
    else if(mode==Mode::Reenter)
    {
        sl::DLSSGState nested;
        framegen::DlssgStateSample sample;
        const auto before=calls;
        const auto result=StreamlineHooks::QueryDlssgState(&Sdk,0,nested,nullptr,
            framegen::DlssgStateQueryReason::Other,&sample);
        Check(result==sl::Result::eErrorInvalidState && !sample.querySequence && calls==before,
            "recursive query never enters SDK or publishes a completion value");
        ++recursiveCalls;
    }
    else
    {
        state.status=sl::DLSSGStatus::eOk;
        state.numFramesActuallyPresented=7;
        if(state.structVersion>=3 && state.structVersion<=4)
        {
            state.inputsProcessingCompletionFence=reinterpret_cast<void*>(poisonFence);
            state.lastPresentInputsProcessingCompletionFenceValue=target;
        }
    }
    SetLastError(sdkError);
    return sl::Result::eOk;
}

framegen::DlssgStateSample Query(sl::DLSSGState& state,bool success=true)
{
    framegen::DlssgStateSample sample;
    const auto before=calls;
    const auto result=StreamlineHooks::QueryDlssgState(&Sdk,0,state,expectedOptions,
        framegen::DlssgStateQueryReason::Provider,&sample);
    const auto error=GetLastError();
    Check(calls==before+1,"one wrapper invocation performs one real query");
    Check((result==sl::Result::eOk)==success && sample.succeeded==success,"original SDK result retained");
    Check(error==sdkError,"diagnostics preserve SDK LastError");
    Check(sample.queryThreadId==GetCurrentThreadId() && sample.querySequence &&
        sample.queryEndNs>=sample.queryBeginNs,"sample belongs to actual query thread and interval");
    return sample;
}

void Layouts()
{
    SYSTEM_INFO system{};GetSystemInfo(&system);
    auto* pages=static_cast<unsigned char*>(VirtualAlloc(nullptr,system.dwPageSize*2,
        MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    Check(pages!=nullptr,"allocate guarded ABI fixture");
    struct Free{void* pointer;~Free(){VirtualFree(pointer,0,MEM_RELEASE);}} free{pages};
    DWORD previous=0;
    Check(VirtualProtect(pages+system.dwPageSize,system.dwPageSize,PAGE_NOACCESS,&previous)!=FALSE,
        "old ABI tail is inaccessible");
    sl::DLSSGState full;
    const auto offset=[&](const void* member){return static_cast<const unsigned char*>(member)-
        reinterpret_cast<const unsigned char*>(&full);};
    for(uint32_t version=1;version<=4;++version)
    {
        const auto bytes=version==1?offset(&full.numFramesToGenerateMax):
            version==2?offset(&full.inputsProcessingCompletionFence):
            version==3?offset(&full.bIsDynamicMFGSupported):static_cast<ptrdiff_t>(sizeof(full));
        for(const auto action:{Mode::Full,Mode::RewriteVersion})
        {
            full={};full.structVersion=version;
            auto* region=pages+system.dwPageSize-bytes;
            std::memcpy(region,&full,static_cast<size_t>(bytes));
            auto& state=*reinterpret_cast<sl::DLSSGState*>(region);
            mode=action;rewriteVersion=4;
            const auto sample=Query(state);
            Check(sample.requestVersion==version,"request ABI saved before SDK mutation");
            Check(sample.inputCompletionFieldsObserved==(version>=3),"v1/v2 never read inaccessible completion tail");
            if(action==Mode::Full && version>=3)
                Check(sample.inputCompletionFenceAddress==poisonFence && sample.inputCompletionFenceValue==target,
                    "numeric poison address and full 64-bit target observed without dereference");
        }
    }
}

void FailureAndMutation()
{
    for(const uint64_t version:{0ull,1ull,2ull,3ull,4ull,5ull,0xFFFFFFFFull,0x100000003ull})
    {
        sl::DLSSGState state;state.structVersion=version;
        state.inputsProcessingCompletionFence=reinterpret_cast<void*>(poisonFence);
        state.lastPresentInputsProcessingCompletionFenceValue=target;
        mode=Mode::Failure;
        auto sample=Query(state,false);
        Check(!sample.inputCompletionFieldsObserved && !sample.inputCompletionFenceAddress &&
            !sample.inputCompletionFenceValue,"failure never publishes stale completion fields");
        mode=Mode::Full;
        sample=Query(state);
        Check(sample.inputCompletionFieldsObserved==(version==3 || version==4),
            "unknown or old requested layouts remain unobserved");
    }
    for(const uint32_t returned:{0u,1u,2u,3u,4u,5u})
    {
        sl::DLSSGState state;
        state.inputsProcessingCompletionFence=reinterpret_cast<void*>(poisonFence);
        state.lastPresentInputsProcessingCompletionFenceValue=0; // Zero is a raw value, not missing data.
        mode=Mode::RewriteVersion;rewriteVersion=returned;
        const auto sample=Query(state);
        Check(sample.inputCompletionFieldsObserved==(returned==3 || returned==4),"returned ABI must also be known");
        if(sample.inputCompletionFieldsObserved)Check(sample.inputCompletionFenceValue==0,"observed zero target retained");
    }
    sl::DLSSGState state;
    mode=Mode::WrongType;
    Check(!Query(state).inputCompletionFieldsObserved,"SDK-mutated struct type prevents tail observation");
    state={};state.structType.data1^=1;mode=Mode::Full;
    Check(!Query(state).inputCompletionFieldsObserved,"wrong requested struct type prevents tail observation");
    state={};mode=Mode::Throw;
    framegen::DlssgStateSample sample;
    bool threw=false;
    try{StreamlineHooks::QueryDlssgState(&Sdk,0,state,nullptr,framegen::DlssgStateQueryReason::Provider,&sample);}
    catch(const std::runtime_error& error){threw=std::strcmp(error.what(),"vendor sentinel")==0;}
    const auto error=GetLastError();
    Check(threw && error==sdkError,"original vendor exception and boundary LastError survive observation");
    Check(!sample.succeeded && !sample.inputCompletionFieldsObserved && sample.querySequence,
        "throwing query retains identity without a false completion value");
    mode=Mode::Full;throwObserver=true;state={};Query(state);throwObserver=false;
    mode=Mode::Reenter;state={};Query(state);
    Check(recursiveCalls==1,"reentrant control was exercised");
    mode=Mode::Full;
    StreamlineHooks::o_slDLSSGGetState=&Sdk;
    for(auto alias:{&StreamlineHooks::hkslDLSSGGetState,&StreamlineHooks::hkslDLSSGGetStateSerial})
    {
        const auto before=calls;state={};
        const auto result=StreamlineHooks::QueryDlssgState(alias,0,state,nullptr,
            framegen::DlssgStateQueryReason::Other,&sample);
        Check(result==sl::Result::eOk && calls==before+1 && sample.api==reinterpret_cast<uintptr_t>(&Sdk),
            "hook alias resolves to exactly one real SDK call");
    }
    const auto before=calls,observed=observations;
    state={};SetLastError(0xAE00);
    const auto result=StreamlineHooks::QueryDlssgState(nullptr,0,state,nullptr,
        framegen::DlssgStateQueryReason::Other,&sample);
    const auto rejectedError=GetLastError();
    Check(result==sl::Result::eErrorInvalidState && calls==before && observations==observed &&
        !sample.querySequence && rejectedError==0xAE00,"rejected null call performs no SDK or observation work");
    sl::DLSSGOptions options;expectedOptions=&options;state={};Query(state);expectedOptions=nullptr;
}

void CacheAndThread()
{
    mode=Mode::Full;
    sl::DLSSGState state;
    const auto fresh=Query(state);
    const auto before=calls;
    for(unsigned i=0;i<1000;++i)
    {
        framegen::DlssgStateSample cached;
        Check(g_dlssgStateQueries.Read(reinterpret_cast<uintptr_t>(&Sdk),0,cached) &&
            cached.querySequence==fresh.querySequence && cached.queryThreadId==fresh.queryThreadId &&
            cached.inputCompletionFenceAddress==poisonFence && cached.inputCompletionFenceValue==target,
            "cache retains original query identity and numeric fields");
    }
    Check(calls==before,"cache reads do not consume SDK counters or completion state");
    framegen::DlssgStateSample worker;
    const auto parentThread=GetCurrentThreadId();
    std::thread thread([&]{sl::DLSSGState other;worker=Query(other);});thread.join();
    Check(worker.queryThreadId && worker.queryThreadId!=parentThread && worker.querySequence>fresh.querySequence,
        "cross-thread publication does not masquerade as parent Present thread");
}
}

int main()
{
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    try
    {
        Layouts();FailureAndMutation();CacheAndThread();
        std::printf("DLSSG state observation PASS: %u checks, %u SDK calls, %u observations\n",checks,calls,observations);
        return 0;
    }
    catch(const std::exception& error){std::fprintf(stderr,"DLSSG state observation FAIL: %s\n",error.what());return 1;}
}
