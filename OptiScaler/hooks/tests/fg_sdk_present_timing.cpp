// The Python test inserts the exact adapter from FG_Hooks.cpp at the marker.
// Compile with the real CPU sampler implementation; no graphics SDK is called.
#include <genshin/GenshinHookCpuTiming.h>
#include <runtime/GuideDiagnostics.h>
#include <cstdio>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace {
// INSERT_PRODUCT_FG_SDK_TIMING

using namespace GenshinHookCpuTiming;
constexpr HRESULT DXGI_ERROR_TEST=static_cast<HRESULT>(0x887A000Au);
unsigned checks=0;
void Check(bool ok,const char* message)
{
    ++checks;
    if(!ok)throw std::runtime_error(message);
}
constexpr unsigned Bucket(unsigned mode=1)
{
    return HookCount*PathCount+static_cast<unsigned>(Stage::FgSdkPresent)*2+((mode&2)?1:0);
}
const Values& Current(unsigned mode=1){return local.values[Bucket(mode)];}
void Reset(unsigned mode=1)
{
    control.store(0);
    local=Local{};
    nextSlot.store(0);
    fgSdkPresentTimingActive=false;
    LARGE_INTEGER hz{};
    Check(QueryPerformanceFrequency(&hz)!=FALSE,"CPU clock is available");
    frequency.store(hz.QuadPart);
    control.store(mode);
}
void DisabledAndUnselected()
{
    for(const unsigned mode:{0u,1u}){
        Reset(mode);
        unsigned calls=0;
        WallSample wall{true,99,100};
        const auto hr=ObserveFgSdkPresent(mode==0,[&]{++calls;return E_FAIL;},&wall);
        Check(hr==E_FAIL && calls==1,"disabled/unselected original executes once with its exact failure");
        Check(!local.attempted && Current()[Calls]==0,"disabled/unselected timing claims no sampler slot");
        Check(!wall.sampled && wall.begin==0 && wall.end==0,"disabled scope returns no fabricated wall endpoints");
        Check(!fgSdkPresentTimingActive,"disabled/unselected call leaves TLS inactive");
    }
}
void ExactNativeBoundary()
{
    Reset();
    unsigned calls=0;
    WallSample wall;
    const auto hr=ObserveFgSdkPresent(true,[&]{
        ++calls;
        Check(fgSdkPresentTimingActive,"native call is inside the TLS interval");
        SetLastError(0x12345678);
        return DXGI_ERROR_TEST; // local HRESULT sentinel, not a graphics call
    },&wall);
    Check(GetLastError()==0x12345678,"timing preserves the original call's LastError");
    Check(hr==DXGI_ERROR_TEST && calls==1,"native failure returned once without translation");
    const auto& row=Current();
    Check(row[Calls]==1 && row[Samples]==1,"one native call creates one sampled stage");
    Check(row[ResultFail]==1 && row[LastResult]==static_cast<uint32_t>(DXGI_ERROR_TEST),"exact HRESULT classified once");
    Check(wall.sampled && wall.begin>0 && wall.end>=wall.begin,"completed sample retains real CPU clock endpoints");
    const auto total=row[Total];
    Check(total==static_cast<uint64_t>(wall.end-wall.begin),"aggregate uses the same native sample endpoints");
    Check(!fgSdkPresentTimingActive,"interval is closed before caller postwork");
    for(unsigned i=0;i<10;++i)Check(row[Total]==total,"caller postwork does not extend the native interval");
}
struct FakePresent {
    unsigned calls=0;
    const void* parameters=nullptr;
    UINT sync=0,flags=0;
    HRESULT Present(UINT s,UINT f){++calls;sync=s;flags=f;return S_FALSE;}
    HRESULT Present1(UINT s,UINT f,const void* p){++calls;sync=s;flags=f;parameters=p;return E_FAIL;}
};
void ExactArguments()
{
    Reset();FakePresent api;int parameters=1;
    Check(ObserveFgSdkPresent(true,[&]{return api.Present(3,0x204);})==S_FALSE,"Present status passes through");
    Check(api.calls==1 && api.sync==3 && api.flags==0x204,"Present keeps sync/flags and call count");
    Check(ObserveFgSdkPresent(true,[&]{return api.Present1(0,0x200,&parameters);})==E_FAIL,"Present1 failure passes through");
    Check(api.calls==2 && api.sync==0 && api.flags==0x200 && api.parameters==&parameters,"Present1 keeps the exact parameter pointer");
    Check(Current()[Calls]==2,"two real API calls produce two stage calls");
}
void SynchronousReentry()
{
    Reset();unsigned outerCalls=0,innerCalls=0;WallSample outer,inner;
    const auto hr=ObserveFgSdkPresent(true,[&]{
        ++outerCalls;
        const auto nested=ObserveFgSdkPresent(true,[&]{++innerCalls;return S_FALSE;},&inner);
        Check(nested==S_FALSE && fgSdkPresentTimingActive,"synchronous callback leaves the outer interval active");
        Check(!inner.sampled,"synchronous nested call is not sampled a second time");
        ObserveFgSdkPresent(false,[&]{Check(fgSdkPresentTimingActive,"unselected nested callback cannot clear the owner");return S_OK;});
        return E_FAIL;
    },&outer);
    Check(hr==E_FAIL && outerCalls==1 && innerCalls==1,"reentry never suppresses or repeats a real callback");
    Check(Current()[Calls]==1 && Current()[Samples]==1,"same-thread inclusive callback counts only the outer API interval");
    Check(Current()[ResultFail]==1 && Current()[ResultPending]==0,"nested result cannot replace the outer HRESULT");
    Check(outer.sampled && !fgSdkPresentTimingActive,"outer interval finishes normally after callback reentry");
    Reset();
    ObserveFgSdkPresent(false,[&]{return ObserveFgSdkPresent(true,[]{return S_OK;});});
    Check(Current()[Calls]==1,"an unobserved caller does not suppress an observed native boundary");
}
void WorkerHasIndependentInterval()
{
    Reset();unsigned workerCalls=0;bool workerSampled=false;Values worker{};DWORD workerThread=0;
    ObserveFgSdkPresent(true,[&]{
        std::thread thread([&]{
            WallSample wall;
            ObserveFgSdkPresent(true,[&]{++workerCalls;return S_OK;},&wall);
            workerSampled=wall.sampled;worker=Current();workerThread=GetCurrentThreadId();
        });
        thread.join();
        Check(fgSdkPresentTimingActive,"independent worker cannot clear source-thread TLS");
        return S_OK;
    });
    Check(workerCalls==1 && workerSampled && workerThread!=GetCurrentThreadId(),"worker original has its own CPU interval");
    Check(Current()[Calls]==1 && worker[Calls]==1 && worker[Samples]==1,"worker and source retain separate per-thread samples");
    Check(nextSlot.load()==2,"existing fixed sampler supplies one slot per actual thread");
}
void ExceptionsAndStride()
{
    Reset();bool caught=false;
    try{ObserveFgSdkPresent(true,[]()->HRESULT {throw std::runtime_error("original failure");});}
    catch(const std::runtime_error& e){caught=std::string_view(e.what())=="original failure";}
    Check(caught && !fgSdkPresentTimingActive,"original exception propagates and restores TLS");
    Check(Current()[Calls]==1 && Current()[ResultFail]==0,"unwind invents no returned HRESULT");
    ObserveFgSdkPresent(true,[]{return S_OK;});
    Check(Current()[Calls]==2,"later call remains observable after unwind");
    Reset();unsigned originals=0;
    for(unsigned i=0;i<65;++i){
        ObserveFgSdkPresent(true,[&]{
            ++originals;
            return ObserveFgSdkPresent(true,[]{return S_OK;});
        });
    }
    Check(originals==65 && Current()[Calls]==65 && Current()[ResultOk]==65,"unsampled outer calls also suppress duplicate nested stage accounting");
    Check(Current()[Samples]==2,"existing stride64 remains unchanged");
    Reset(3);
    ObserveFgSdkPresent(true,[]{return S_OK;});
    Check(Current(3)[Calls]==1 && Current(1)[Calls]==0,"existing Active snapshot selects the original sampler path");
}
}
int main()
{
    try {
        DisabledAndUnselected();ExactNativeBoundary();ExactArguments();SynchronousReentry();
        WorkerHasIndependentInterval();ExceptionsAndStride();
        std::printf("PASS %u CPU sampler/adapter checks; no SDK, GPU or pacing executed\n",checks);
    } catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
