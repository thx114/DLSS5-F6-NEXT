// The runner inserts the production QueryDlssgState body. Real SDK headers and
// Windows ABI/LastError and real WARP fences/queues; no game or SDK DLL.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <sl_dlss_g.h>
#include <framegen/DlssgStateSample.h>
#include <framegen/Dx12InputFence.h>
#include <framegen/Dx12InputPresent.h>
#include <framegen/dlssg/DlssgTelemetry.h>
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>
#include <mutex>
#include <functional>

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
        framegen::DlssgStateSample* =nullptr,framegen::Dx12InputFence* =nullptr);
    static sl::Result hkslDLSSGGetState(const sl::ViewportHandle& v,sl::DLSSGState& s,const sl::DLSSGOptions* o)
    {return QueryDlssgState(o_slDLSSGGetState,v,s,o,framegen::DlssgStateQueryReason::Game);}
    static sl::Result hkslDLSSGGetStateSerial(const sl::ViewportHandle& v,sl::DLSSGState& s,const sl::DLSSGOptions* o)
    {return QueryDlssgState(o_slDLSSGGetState,v,s,o,framegen::DlssgStateQueryReason::Other);}
};

// INSERT_PRODUCTION_QUERY

namespace
{
enum class Mode { Full, Failure, Throw, RewriteVersion, WrongType, Reenter, LiveFence, ReturnNoError, ThrowNoError };
Mode mode=Mode::Full;
uint32_t rewriteVersion=4;
constexpr DWORD sdkError=0xA123;
constexpr uintptr_t poisonFence=0xDEAD0010;
constexpr uint64_t target=0xFEDCBA9876543210ull;
unsigned recursiveCalls=0;
const sl::DLSSGOptions* expectedOptions=nullptr;
IUnknown* liveFence=nullptr;
uint64_t liveValue=17;
uint32_t liveVersion=4;
bool liveWrongType=false,liveNext=false;
std::function<void()> sdkCallback;

sl::Result Sdk(const sl::ViewportHandle&,sl::DLSSGState& state,const sl::DLSSGOptions* options)
{
    ++calls;
    if (sdkCallback) sdkCallback();
    Check(options==expectedOptions,"original options pointer reaches SDK unchanged");
    if(mode==Mode::ReturnNoError)return sl::Result::eErrorInvalidState;
    if(mode==Mode::ThrowNoError)throw std::runtime_error("no-error vendor sentinel");
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
    else if(mode==Mode::LiveFence)
    {
        state.structVersion=liveVersion;
        if(liveWrongType)state.structType.data1^=1;
        if(liveNext)state.next=&state;
        state.inputsProcessingCompletionFence=liveFence;
        state.lastPresentInputsProcessingCompletionFenceValue=liveValue;
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
            if(version<=2)
            {
                std::memcpy(region,&full,static_cast<size_t>(bytes));
                framegen::Dx12InputFence owning;
                framegen::DlssgStateSample owningSample;
                const auto result=StreamlineHooks::QueryDlssgState(&Sdk,0,state,nullptr,
                    framegen::DlssgStateQueryReason::CompletionFence,&owningSample,&owning);
                Check(result==sl::Result::eOk && !owning && !owningSample.inputCompletionFieldsObserved,
                    "owning preflight never reads inaccessible v1/v2 completion tail, even after SDK rewrites version");
            }
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

// Real WARP fence plus a counting COM boundary: prove that the production
// wrapper owns exactly its QI reference, captures while its gate is active,
// and preserves SDK errors even when COM mutates LastError.
struct CountedFence final : ID3D12Fence
{
    Microsoft::WRL::ComPtr<ID3D12Fence> actual;
    unsigned refs=1,queries=0;
    bool offerFence=true,reenter=false,throwGetDevice=false;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override
    {
        ++queries;*out=nullptr;
        if(reenter)
        {
            reenter=false;const auto previousCalls=calls;
            sl::DLSSGState state;framegen::DlssgStateSample sample;framegen::Dx12InputFence captured;
            const auto result=StreamlineHooks::QueryDlssgState(&Sdk,0,state,nullptr,
                framegen::DlssgStateQueryReason::CompletionFence,&sample,&captured);
            Check(result==sl::Result::eErrorInvalidState && !captured && !sample.querySequence && calls==previousCalls,
                "owning QI occurs inside the original query gate; recursive SDK call refused");
        }
        SetLastError(0xBAD2);
        if(iid==__uuidof(IUnknown) || (offerFence && iid==__uuidof(ID3D12Fence)))
        { *out=static_cast<ID3D12Fence*>(this);AddRef();return S_OK; }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return ++refs;}
    ULONG STDMETHODCALLTYPE Release() override {SetLastError(0xBAD3);return --refs;}
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid,UINT* size,void* data) override
    {return actual->GetPrivateData(guid,size,data);}
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid,UINT size,const void* data) override
    {return actual->SetPrivateData(guid,size,data);}
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid,const IUnknown* data) override
    {return actual->SetPrivateDataInterface(guid,data);}
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR name) override {return actual->SetName(name);}
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID iid,void** device) override
    {
        if (throwGetDevice) throw std::runtime_error("fence device validation failed");
        return actual->GetDevice(iid,device);
    }
    UINT64 STDMETHODCALLTYPE GetCompletedValue() override {return actual->GetCompletedValue();}
    HRESULT STDMETHODCALLTYPE SetEventOnCompletion(UINT64 value,HANDLE event) override
    {return actual->SetEventOnCompletion(value,event);}
    HRESULT STDMETHODCALLTYPE Signal(UINT64 value) override {return actual->Signal(value);}
};

void OwningFence()
{
    using Microsoft::WRL::ComPtr;
    ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter> warp;ComPtr<ID3D12Device> device;
    Check(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory))==S_OK &&
        factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))==S_OK &&
        D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))==S_OK,"real WARP fence device");
    CountedFence fence;
    Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence.actual))==S_OK,"real input fence");
    liveFence=&fence;mode=Mode::LiveFence;
    framegen::Dx12InputFence owned;
    const auto capture=[&](sl::DLSSGState& state,framegen::DlssgStateQueryReason reason=framegen::DlssgStateQueryReason::CompletionFence) {
        framegen::DlssgStateSample sample;
        const auto result=StreamlineHooks::QueryDlssgState(&Sdk,0,state,expectedOptions,reason,&sample,&owned);
        Check(GetLastError()==sdkError,"owning COM operations preserve original SDK LastError");
        return result;
    };
    for(const auto version:{3u,4u})
    {
        sl::DLSSGState state;state.structVersion=version;liveVersion=version;
        Check(capture(state)==sl::Result::eOk && owned && owned.Value()==17 && fence.refs==2,
            "successful v3/v4 capture owns one reference to the actual returned fence");
        ComPtr<ID3D12Device> owner;
        Check(owned.Get()->GetDevice(IID_PPV_ARGS(&owner))==S_OK && owner.Get()==device.Get(),"held fence retains its real device");
        Check(owned.Get()->GetCompletedValue()==0,"holding a future target does not claim GPU completion");
        framegen::Dx12InputFence moved(std::move(owned));
        Check(moved && !owned && !owned.Value() && fence.refs==2,"move transfers the one QI reference");
        owned=std::move(moved);owned.Reset();Check(fence.refs==1,"Reset releases only the QI-owned reference");
    }
    liveVersion=4;fence.reenter=true;sl::DLSSGState state;
    capture(state);Check(owned && !fence.reenter && fence.refs==2,"recursive capture leaves the outer owning query valid");
    state={};mode=Mode::Failure;
    Check(capture(state)==sl::Result::eErrorInvalidState && !owned && fence.refs==1,
        "failed query revokes an older output rather than returning stale ownership");
    mode=Mode::LiveFence;
    for(const uint64_t requested:{0ull,1ull,2ull,5ull,0x100000003ull})
    {
        state={};state.structVersion=requested;const auto queries=fence.queries;
        capture(state);Check(!owned && fence.queries==queries,"old/unknown request never dereferences a returned fence");
    }
    for(const auto returned:{0u,1u,2u,5u})
    {
        state={};liveVersion=returned;const auto queries=fence.queries;
        capture(state);Check(!owned && fence.queries==queries,"old/unknown response never dereferences a returned fence");
    }
    liveVersion=4;liveWrongType=true;state={};capture(state);Check(!owned,"rewritten type rejects ownership");liveWrongType=false;
    liveNext=true;state={};capture(state);Check(!owned,"SDK extension chain rejects ownership");liveNext=false;
    state={};state.next=&state;capture(state);Check(!owned,"request extension chain rejects ownership");
    state={};state.inputsProcessingCompletionFence=&fence;capture(state);Check(!owned,"preseeded pointer cannot authenticate an SDK write");
    state={};state.lastPresentInputsProcessingCompletionFenceValue=17;capture(state);Check(!owned,"preseeded target rejects owning capture");
    state={};capture(state,framegen::DlssgStateQueryReason::Provider);Check(!owned,"ordinary provider query stays numeric-only");
    sl::DLSSGOptions options;expectedOptions=&options;state={};capture(state);Check(!owned,"SetOptions-in-query cannot capture an input fence");expectedOptions=nullptr;
    liveValue=UINT64_MAX;state={};capture(state);Check(!owned && owned.Status()==E_INVALIDARG,"device-loss sentinel target is not a reusable dependency");
    liveValue=0;state={};capture(state);Check(owned && !owned.Value(),"zero target remains an owned raw fence, not a Present association");owned.Reset();
    liveValue=17;fence.offerFence=false;state={};capture(state);Check(!owned && owned.Status()==E_NOINTERFACE && fence.refs==1,"QI failure retains no SDK-borrowed reference");fence.offerFence=true;
    liveFence=nullptr;state={};capture(state);Check(!owned && owned.Status()==E_POINTER,"missing borrowed fence remains missing");
    liveFence=&fence;state={};capture(state);mode=Mode::Throw;state={};bool threw=false;
    try{capture(state);}catch(const std::runtime_error&){threw=true;}
    Check(threw && GetLastError()==sdkError && !owned && fence.refs==1,"SDK exception clears old ownership and preserves exception error");
    mode=Mode::LiveFence;state={};capture(state);
    SetLastError(0x1234);framegen::DlssgStateSample sample;
    Check(StreamlineHooks::QueryDlssgState(nullptr,0,state,nullptr,framegen::DlssgStateQueryReason::CompletionFence,&sample,&owned)==sl::Result::eErrorInvalidState &&
        GetLastError()==0x1234 && !owned && fence.refs==1,"rejected query releases prior capture without changing entry LastError");
    for(const auto action:{Mode::ReturnNoError,Mode::ThrowNoError})
    {
        mode=Mode::LiveFence;state={};capture(state);Check(fence.refs==2,"no-error SDK case starts with an owned output");
        mode=action;state={};bool failed=false;SetLastError(0xE321);
        try{(void)StreamlineHooks::QueryDlssgState(&Sdk,0,state,nullptr,framegen::DlssgStateQueryReason::CompletionFence,&sample,&owned);}
        catch(const std::runtime_error&){failed=true;}
        Check(GetLastError()==0xE321 && failed==(action==Mode::ThrowNoError) && !owned && fence.refs==1,
            "releasing previous capture cannot replace SDK entry LastError when SDK leaves it unchanged");
    }
    mode=Mode::Full;liveFence=nullptr;
}

void BoundPresent()
{
    using namespace framegen::input_present;
    using Microsoft::WRL::ComPtr;
    ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter> warp;ComPtr<ID3D12Device> device;
    Check(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory))==S_OK && factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))==S_OK &&
        D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))==S_OK,"bound Present WARP device");
    D3D12_RESOURCE_DESC desc {};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=4;
    desc.Height=desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES heap {};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> buffer;
    Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,
        IID_PPV_ARGS(&buffer))==S_OK,"exact buffer owner for transaction");
    CountedFence fence;Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence.actual))==S_OK,"bound SDK fence");
    mode=Mode::LiveFence;liveFence=&fence;liveValue=17;liveVersion=4;
    Binding binding {1,1,1,1,0xCAFE,ComIdentity::Get(buffer.Get()),reinterpret_cast<uintptr_t>(&Sdk),0,GetCurrentThreadId()};
    auto currentBinding=binding;
    Domain domain;
    unsigned presents=0,queries=0;
    const auto current=[&]{return currentBinding;};
    const auto present=[&]{++presents;SetLastError(0xA543);return S_OK;};
    const auto query=[&](framegen::Dx12InputFence& owned,framegen::DlssgStateSample& sample) {
        ++queries;Check(presents>0 && domain.Active(),"owning query follows a real SDK Present inside its active transaction");
        sl::DLSSGState state;
        return StreamlineHooks::QueryDlssgState(&Sdk,0,state,nullptr,
            framegen::DlssgStateQueryReason::CompletionFence,&sample,&owned);
    };
    {
        auto result=Invoke(domain,binding,buffer.Get(),present,current,query);
        Check(GetLastError()==0xA543 && result.presentResult==S_OK && result.forwarded && result.completion &&
            result.completion.Buffer()==buffer.Get() && result.completion.Owner()==binding &&
            result.completion.PresentSerial() && result.completion.QuerySequence() && !domain.Active() && fence.refs==2,
            "SDK return, actual buffer/epoch/thread/query and owning fence join into one receipt");
        Check(result.completion.Poll()==Completion::Retirement::Pending,"held pending SDK fence is not completion");
        auto moved=std::move(result.completion);
        Check(moved && !result.completion && moved.FenceValue()==17 && fence.refs==2,"bound receipt moves without duplicated ownership");
    }
    Check(fence.refs==1,"bound receipt releases only its acquired fence reference");
    for(const HRESULT status:{E_FAIL,DXGI_ERROR_WAS_STILL_DRAWING,DXGI_STATUS_OCCLUDED})
    {
        const auto before=queries;
        auto result=Invoke(domain,binding,buffer.Get(),[&]{++presents;SetLastError(0xA543);return status;},current,query);
        Check(result.forwarded && result.presentResult==status && !result.completion && queries==before &&
            !domain.Active() && GetLastError()==0xA543,"failed/busy/status Present never associates an earlier SDK target");
    }
    currentBinding.bufferEpoch=2;const auto oldPresents=presents;
    auto rejected=Invoke(domain,binding,buffer.Get(),present,current,query);
    Check(!rejected.forwarded && !rejected.completion && presents==oldPresents,"stale buffer epoch refuses SDK invocation");
    currentBinding=binding;
    {
        const auto before=queries;
        auto result=Invoke(domain,binding,buffer.Get(),[&] {
            ++presents;auto nested=Invoke(domain,binding,buffer.Get(),present,current,query);
            Check(!nested.forwarded && !nested.completion,"recursive source Present is refused before SDK");
            SetLastError(0xA543);return S_OK;
        },current,query);
        Check(result.forwarded && !result.completion && queries==before && !domain.Active(),
            "nested attempt revokes the outer association rather than stealing its query");
    }
    {
        auto result=Invoke(domain,binding,buffer.Get(),present,current,[&](auto& owned,auto& sample) {
            const auto status=query(owned,sample);++currentBinding.sdkEpoch;return status;
        });
        Check(!result.completion && !domain.Active() && fence.refs==1,"SDK callback epoch replacement cannot publish to a new owner");
        currentBinding=binding;
    }
    for(const auto wrong:{0,1,2,3})
    {
        auto result=Invoke(domain,binding,buffer.Get(),present,current,[&](auto& owned,auto& sample) {
            const auto status=query(owned,sample);
            if(wrong==0)++sample.queryThreadId;
            if(wrong==1)++sample.api;
            if(wrong==2)++sample.viewport;
            if(wrong==3)sample.reason=framegen::DlssgStateQueryReason::Provider;
            return status;
        });
        Check(!result.completion && !domain.Active() && fence.refs==1,"wrong query thread/API/viewport/reason cannot authorize this buffer");
    }
    {
        auto result=Invoke(domain,binding,buffer.Get(),present,current,[&](auto& owned,auto& sample)->sl::Result {
            query(owned,sample);throw std::runtime_error("post-Present query failed");
        });
        Check(result.forwarded && result.presentResult==S_OK && !result.completion && !domain.Active() &&
            fence.refs==1 && GetLastError()==0xA543,"throwing post-Present query retains SDK result/error and grants no ownership");
    }
    bool threw=false;
    {
        fence.throwGetDevice=true;
        auto result=Invoke(domain,binding,buffer.Get(),present,current,query);
        fence.throwGetDevice=false;
        Check(result.forwarded && result.presentResult==S_OK && !result.completion && !domain.Active() &&
            fence.refs==1 && GetLastError()==0xA543,
            "exception after a successful query cannot publish a partially validated receipt");
    }
    try {Invoke(domain,binding,buffer.Get(),[&]()->HRESULT{SetLastError(0xEE12);throw std::runtime_error("actual Present failed");},current,query);}
    catch(const std::runtime_error&){threw=true;}
    Check(threw && !domain.Active() && GetLastError()==0xEE12,"actual SDK Present exception unwinds its association and original error");
    liveValue=0;
    auto zero=Invoke(domain,binding,buffer.Get(),present,current,query);
    Check(zero.forwarded && !zero.completion && fence.refs==1,"zero target owns no consumed Present identity");liveValue=17;

    // Two actual queues: final writer -> gated SDK reader -> next producer.
    // A successful queue Wait does not retire the pinned input on the CPU.
    ComPtr<ID3D12CommandQueue> producer,reader;
    D3D12_COMMAND_QUEUE_DESC queueDesc {};
    Check(device->CreateCommandQueue(&queueDesc,IID_PPV_ARGS(&producer))==S_OK &&
        device->CreateCommandQueue(&queueDesc,IID_PPV_ARGS(&reader))==S_OK,"two actual queue owners");
    ComPtr<ID3D12Fence> gate,writerDone;
    Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate))==S_OK &&
        device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&writerDone))==S_OK,"actual gate and writer timelines");
    // Open every queue gate before releasing its resources, including an
    // assertion/exception path. Normal retirement still verifies real targets.
    struct DrainOnExit {
        ID3D12Fence* gate;
        ID3D12Device* device;
        ID3D12CommandQueue* producer;
        ID3D12CommandQueue* reader;
        ~DrainOnExit()
        {
            gate->Signal(1);
            for(auto* queue:{producer,reader})
            {
                ComPtr<ID3D12Fence> done;
                HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
                const bool created=event && device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&done))==S_OK;
                const bool submitted=created && queue->Signal(done.Get(),1)==S_OK;
                const bool registered=submitted && done->SetEventOnCompletion(1,event)==S_OK;
                const bool completed=registered && WaitForSingleObject(event,5000)==WAIT_OBJECT_0 && done->GetCompletedValue()>=1 &&
                    done->GetCompletedValue()!=UINT64_MAX;
                if(event)CloseHandle(event);
                if(!completed)std::terminate(); // Never unwind in-flight fixture operands.
            }
        }
    };
    heap.Type=D3D12_HEAP_TYPE_UPLOAD;desc.Width=8;
    ComPtr<ID3D12Resource> upload;
    Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,
        IID_PPV_ARGS(&upload))==S_OK,"distinct old/new uploaded source values");
    void* data=nullptr;Check(upload->Map(0,nullptr,&data)==S_OK,"map two source values");
    const unsigned values[] {0x11223344,0x55667788};std::memcpy(data,values,sizeof(values));upload->Unmap(0,nullptr);
    heap.Type=D3D12_HEAP_TYPE_READBACK;
    ComPtr<ID3D12Resource> readback;
    Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,
        IID_PPV_ARGS(&readback))==S_OK,"actual SDK-reader and final output readback");
    struct Recording {ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;};
    std::vector<Recording> recordings;
    const auto record=[&]()->ID3D12GraphicsCommandList* {
        recordings.emplace_back();auto& item=recordings.back();
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&item.allocator))==S_OK &&
            device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,item.allocator.Get(),nullptr,IID_PPV_ARGS(&item.list))==S_OK,
            "independent actual command-list/allocator recording");return item.list.Get();
    };
    const auto submit=[&](ID3D12CommandQueue* queue,ID3D12GraphicsCommandList* list) {
        Check(list->Close()==S_OK,"close actual recording");ID3D12CommandList* lists[] {list};queue->ExecuteCommandLists(1,lists);
    };
    const auto transition=[&](ID3D12GraphicsCommandList* list,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER barrier {};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition={buffer.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};list->ResourceBarrier(1,&barrier);
    };
    const auto wait=[&](ID3D12Fence* done,uint64_t value) {
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);Check(event!=nullptr,"completion event");
        const auto hr=done->SetEventOnCompletion(value,event);const auto status=SUCCEEDED(hr)?WaitForSingleObject(event,5000):WAIT_FAILED;
        CloseHandle(event);Check(hr==S_OK && status==WAIT_OBJECT_0 && done->GetCompletedValue()!=UINT64_MAX &&
            done->GetCompletedValue()>=value,"exact actual owner timeline completed");
    };
    DrainOnExit drainOnExit {gate.Get(),device.Get(),producer.Get(),reader.Get()};
    auto* first=record();first->CopyBufferRegion(buffer.Get(),0,upload.Get(),0,4);
    transition(first,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);submit(producer.Get(),first);
    Check(producer->Signal(writerDone.Get(),1)==S_OK,"publish final writer dependency");
    auto* sdk=record();transition(sdk,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
    sdk->CopyBufferRegion(readback.Get(),0,buffer.Get(),0,4);
    transition(sdk,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
    liveFence=fence.actual.Get(); // Native queue Wait requires the actual runtime fence, not the counting COM proxy.
    auto captured=Invoke(domain,binding,buffer.Get(),[&] {
        ++presents;Check(reader->Wait(writerDone.Get(),1)==S_OK && reader->Wait(gate.Get(),1)==S_OK,"SDK reader orders final writer and real gate");
        submit(reader.Get(),sdk);Check(reader->Signal(fence.actual.Get(),17)==S_OK,"SDK publishes actual reader fence after its input read");
        SetLastError(0xA543);return S_OK;
    },current,query);
    Check(captured.completion && captured.completion.Poll()==Completion::Retirement::Pending,"pending real SDK reader receipt");
    auto wrongBinding=binding;++wrongBinding.bufferEpoch;
    Check(captured.completion.OrderBeforeWrite(wrongBinding,buffer.Get(),producer.Get())==E_INVALIDARG,
        "old-generation reader receipt cannot authorize a replacement buffer write");
    Check(captured.completion.OrderBeforeWrite(binding,buffer.Get(),producer.Get())==S_OK &&
        captured.completion.Poll()==Completion::Retirement::Pending,"successful producer Wait is order only, not CPU retirement");
    auto* next=record();transition(next,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
    next->CopyBufferRegion(buffer.Get(),0,upload.Get(),4,4);
    transition(next,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);
    next->CopyBufferRegion(readback.Get(),4,buffer.Get(),0,4);
    transition(next,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);submit(producer.Get(),next);
    Check(producer->Signal(writerDone.Get(),2)==S_OK,"publish next actual producer completion");
    HANDLE blocked=CreateEventW(nullptr,FALSE,FALSE,nullptr);Check(blocked!=nullptr,"delayed writer event");
    const auto blockedHr=writerDone->SetEventOnCompletion(2,blocked);
    const auto blockedStatus=SUCCEEDED(blockedHr)?WaitForSingleObject(blocked,200):WAIT_FAILED;
    CloseHandle(blocked);
    Check(blockedHr==S_OK && blockedStatus==WAIT_TIMEOUT && writerDone->GetCompletedValue()<2,
        "SDK reader gate keeps the independent producer from overwriting its input");
    Check(gate->Signal(1)==S_OK,"release SDK reader gate");wait(writerDone.Get(),2);
    Check(captured.completion.Poll()==Completion::Retirement::Complete,"SDK input retires only on its actual completed fence");
    Check(readback->Map(0,nullptr,&data)==S_OK,"map completed queue outputs");unsigned seen[2] {};std::memcpy(seen,data,sizeof(seen));readback->Unmap(0,nullptr);
    Check(seen[0]==values[0] && seen[1]==values[1],"SDK consumes the original picture before the independent producer writes the new one");
    captured={};
    ComPtr<ID3D12InfoQueue> info;Check(device.As(&info)==S_OK,"real D3D12 validation queue");
    for(UINT64 i=0;i<info->GetNumStoredMessagesAllowedByRetrievalFilter();++i)
    {
        SIZE_T bytes=0;Check(SUCCEEDED(info->GetMessage(i,nullptr,&bytes)),"debug message size");
        std::vector<unsigned char> storage(bytes);auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        Check(info->GetMessage(i,message,&bytes)==S_OK,"debug message data");
        Check(message->Severity!=D3D12_MESSAGE_SEVERITY_ERROR && message->Severity!=D3D12_MESSAGE_SEVERITY_CORRUPTION,
            "bound Present / two queue dependency has no D3D12 debug errors");
    }
    mode=Mode::Full;liveFence=nullptr;
}
// INSERT_PRODUCTION_INPUT_FIXTURE
}

int main()
{
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    try
    {
        Microsoft::WRL::ComPtr<ID3D12Debug> debug;
        Check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))==S_OK,"D3D12 debug layer available");debug->EnableDebugLayer();
        Layouts();FailureAndMutation();CacheAndThread();OwningFence();BoundPresent();ProductionInputPresent();
        std::printf("DLSSG state observation PASS: %u checks, %u SDK calls, %u observations\n",checks,calls,observations);
        return 0;
    }
    catch(const std::exception& error){std::fprintf(stderr,"DLSSG state observation FAIL: %s\n",error.what());return 1;}
}
