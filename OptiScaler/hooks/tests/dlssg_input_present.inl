// Included by the SDK boundary fixture. The runner inserts the real provider
// methods/template; only the provider environment and DXGI chain are scripted.
enum class FGOutput { DLSSG, Other };
class DLSSG_Dx12;
struct State
{
    DLSSG_Dx12* currentFG=nullptr;
    bool isShuttingDown=false,externalFrameGeneration=false;
    FGOutput activeFgOutput=FGOutput::DLSSG;
    static State& Instance() {static State value;return value;}
};
struct StreamlineProxy
{
    static bool IsD3D12Inited() {return true;}
    static decltype(&slDLSSGGetState) DLSSGGetState() {return &Sdk;}
};

struct InputResource final : ID3D12Resource
{
    Microsoft::WRL::ComPtr<ID3D12Resource> actual;
    unsigned refs=1;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override
    {
        *out=nullptr;
        if (iid!=__uuidof(IUnknown) && iid!=__uuidof(ID3D12Resource)) return E_NOINTERFACE;
        *out=static_cast<ID3D12Resource*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return ++refs;}
    ULONG STDMETHODCALLTYPE Release() override {SetLastError(0xBAD8);return --refs;}
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid,UINT* size,void* data) override
    {return actual->GetPrivateData(guid,size,data);}
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid,UINT size,const void* data) override
    {return actual->SetPrivateData(guid,size,data);}
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid,const IUnknown* data) override
    {return actual->SetPrivateDataInterface(guid,data);}
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR name) override {return actual->SetName(name);}
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID iid,void** device) override {return actual->GetDevice(iid,device);}
    HRESULT STDMETHODCALLTYPE Map(UINT index,const D3D12_RANGE* range,void** data) override {return actual->Map(index,range,data);}
    void STDMETHODCALLTYPE Unmap(UINT index,const D3D12_RANGE* range) override {actual->Unmap(index,range);}
    D3D12_RESOURCE_DESC STDMETHODCALLTYPE GetDesc() override {return actual->GetDesc();}
    D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE GetGPUVirtualAddress() override {return actual->GetGPUVirtualAddress();}
    HRESULT STDMETHODCALLTYPE WriteToSubresource(UINT index,const D3D12_BOX* box,const void* data,UINT row,UINT depth) override
    {return actual->WriteToSubresource(index,box,data,row,depth);}
    HRESULT STDMETHODCALLTYPE ReadFromSubresource(void* data,UINT row,UINT depth,UINT index,const D3D12_BOX* box) override
    {return actual->ReadFromSubresource(data,row,depth,index,box);}
    HRESULT STDMETHODCALLTYPE GetHeapProperties(D3D12_HEAP_PROPERTIES* properties,D3D12_HEAP_FLAGS* flags) override
    {return actual->GetHeapProperties(properties,flags);}
};

struct InputChain final : IDXGISwapChain3
{
    Microsoft::WRL::ComPtr<ID3D12Resource> buffers[2];
    InputResource proxies[2];
    bool useProxy=false;
    unsigned refs=1,index=0,bufferReads=0,presents=0;
    HRESULT bufferResult=S_OK,presentResult=S_OK;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override
    {
        *out=nullptr;SetLastError(0xBAD4);
        if (iid!=__uuidof(IUnknown) && iid!=__uuidof(IDXGISwapChain3)) return E_NOINTERFACE;
        *out=static_cast<IDXGISwapChain3*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return ++refs;}
    ULONG STDMETHODCALLTYPE Release() override {SetLastError(0xBAD5);return --refs;}
    HRESULT STDMETHODCALLTYPE Present(UINT,UINT) override
    {++presents;index^=1;SetLastError(0xA789);return presentResult;}
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT buffer,REFIID iid,void** out) override
    {
        ++bufferReads;*out=nullptr;SetLastError(0xBAD6);
        if (bufferResult!=S_OK || buffer>=2) return E_FAIL;
        return useProxy?proxies[buffer].QueryInterface(iid,out):buffers[buffer]->QueryInterface(iid,out);
    }
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override {return index;}
#define INPUT_CHAIN_STUB(name,params) HRESULT STDMETHODCALLTYPE name params override {return E_NOTIMPL;}
    INPUT_CHAIN_STUB(SetPrivateData,(REFGUID,UINT,const void*))
    INPUT_CHAIN_STUB(SetPrivateDataInterface,(REFGUID,const IUnknown*))
    INPUT_CHAIN_STUB(GetPrivateData,(REFGUID,UINT*,void*))
    INPUT_CHAIN_STUB(GetParent,(REFIID,void**))
    INPUT_CHAIN_STUB(GetDevice,(REFIID,void**))
    INPUT_CHAIN_STUB(SetFullscreenState,(BOOL,IDXGIOutput*))
    INPUT_CHAIN_STUB(GetFullscreenState,(BOOL*,IDXGIOutput**))
    INPUT_CHAIN_STUB(GetDesc,(DXGI_SWAP_CHAIN_DESC*))
    INPUT_CHAIN_STUB(ResizeBuffers,(UINT,UINT,UINT,DXGI_FORMAT,UINT))
    INPUT_CHAIN_STUB(ResizeTarget,(const DXGI_MODE_DESC*))
    INPUT_CHAIN_STUB(GetContainingOutput,(IDXGIOutput**))
    INPUT_CHAIN_STUB(GetFrameStatistics,(DXGI_FRAME_STATISTICS*))
    INPUT_CHAIN_STUB(GetLastPresentCount,(UINT*))
    INPUT_CHAIN_STUB(GetDesc1,(DXGI_SWAP_CHAIN_DESC1*))
    INPUT_CHAIN_STUB(GetFullscreenDesc,(DXGI_SWAP_CHAIN_FULLSCREEN_DESC*))
    INPUT_CHAIN_STUB(GetHwnd,(HWND*))
    INPUT_CHAIN_STUB(GetCoreWindow,(REFIID,void**))
    INPUT_CHAIN_STUB(Present1,(UINT,UINT,const DXGI_PRESENT_PARAMETERS*))
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override {return FALSE;}
    INPUT_CHAIN_STUB(GetRestrictToOutput,(IDXGIOutput**))
    INPUT_CHAIN_STUB(SetBackgroundColor,(const DXGI_RGBA*))
    INPUT_CHAIN_STUB(GetBackgroundColor,(DXGI_RGBA*))
    INPUT_CHAIN_STUB(SetRotation,(DXGI_MODE_ROTATION))
    INPUT_CHAIN_STUB(GetRotation,(DXGI_MODE_ROTATION*))
    INPUT_CHAIN_STUB(SetSourceSize,(UINT,UINT))
    INPUT_CHAIN_STUB(GetSourceSize,(UINT*,UINT*))
    INPUT_CHAIN_STUB(SetMaximumFrameLatency,(UINT))
    INPUT_CHAIN_STUB(GetMaximumFrameLatency,(UINT*))
    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override {return nullptr;}
    INPUT_CHAIN_STUB(SetMatrixTransform,(const DXGI_MATRIX_3X2_F*))
    INPUT_CHAIN_STUB(GetMatrixTransform,(DXGI_MATRIX_3X2_F*))
    INPUT_CHAIN_STUB(CheckColorSpaceSupport,(DXGI_COLOR_SPACE_TYPE,UINT*))
    INPUT_CHAIN_STUB(SetColorSpace1,(DXGI_COLOR_SPACE_TYPE))
    INPUT_CHAIN_STUB(ResizeBuffers1,(UINT,UINT,UINT,DXGI_FORMAT,UINT,const UINT*,IUnknown* const*))
#undef INPUT_CHAIN_STUB
};

class DLSSG_Dx12
{
  public:
    struct Owner
    {
        uint64_t instance=3,lifecycle=7;
        uintptr_t getState=reinterpret_cast<uintptr_t>(&Sdk);
        uint32_t viewport=0;
        bool known=true;
        bool Known() const {return known;}
    } ownerState;
    Owner CurrentProviderOwner() const {return ownerState;}
    framegen::input_present::Domain _inputPresentDomain;
    std::recursive_mutex _inputContinuityMutex;
    uint64_t _inputBufferEpoch=1;
    uint64_t _inputOwnerEpoch=1;
    ULONGLONG _inputPresentSampleAt=0;
    bool _providerStateQueryActive=false,_isActive=true;
    bool _ownerChanging=false,_hookRetiring=false,_teardownPending=false,_teardownCompleting=false,_teardownQuarantined=false;
    unsigned _presentExecutionDepth=1;
    struct {bool active=false;bool InFlight() const {return active;}} _dispatchOptions;
    IDXGISwapChain* _swapChain=nullptr;
    sl::ViewportHandle viewport{0};
    struct InputPresentObservation
    {
        framegen::input_present::Binding binding;
        Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
        bool refused=false;
    };
    InputPresentObservation PrepareInputPresentObservation(IDXGISwapChain*,UINT);
    framegen::input_present::Binding CurrentInputPresentBinding(IDXGISwapChain*,ID3D12Resource*);
    sl::Result QueryInputPresentCompletion(framegen::Dx12InputFence&,framegen::DlssgStateSample&);
    unsigned recorded=0;
    bool joined=false;
    uintptr_t observedInput=0;
    uint64_t querySequence=0;
    void RecordInputPresentObservation(const InputPresentObservation& observation,
        const framegen::input_present::Result& result,const framegen::DlssgStateSample& sample) noexcept
    {
        ++recorded;joined=bool(result.completion);observedInput=observation.binding.buffer;querySequence=sample.querySequence;
        SetLastError(0xBAD7);
    }
    bool BeginInputBufferRebuild();
    // INSERT_PRODUCTION_INPUT_TEMPLATE
};

// INSERT_PRODUCTION_INPUT_METHODS

void ProductionInputPresent()
{
    using namespace framegen::input_present;
    using Microsoft::WRL::ComPtr;
    ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter> warp;ComPtr<ID3D12Device> device;
    Check(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory))==S_OK && factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))==S_OK &&
        D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))==S_OK,"provider observer WARP device");
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=4;
    desc.Height=desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    InputChain chain;
    for (auto& buffer:chain.buffers)
        Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,
            IID_PPV_ARGS(&buffer))==S_OK,"two distinct SDK proxy buffers");
    CountedFence fence;Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence.actual))==S_OK,"provider SDK input fence");
    mode=Mode::LiveFence;liveFence=&fence;liveValue=31;liveVersion=4;
    DLSSG_Dx12 provider;provider._swapChain=&chain;State::Instance().currentFG=&provider;
    const auto invoke=[&](auto&& present,UINT flags=0,Api api=Api::Present) {
        SetLastError(0x1234);
        const auto result=provider.ObserveInputPresent(&chain,flags,api,std::forward<decltype(present)>(present));
        Check(GetLastError()==0xA789,"real provider preserves original Present LastError across owned COM/record cleanup");
        return result;
    };
    const auto real=[&]{Check(GetLastError()==0x1234,"SDK entry LastError survives observation preflight");return chain.Present(0,0);};
    const auto before=calls;
    Check(invoke(real)==S_OK && provider.joined && calls==before+1 && chain.presents==1 && chain.index==1 &&
        provider.observedInput==ComIdentity::Get(chain.buffers[0].Get()) && provider.querySequence && chain.refs==1 && fence.refs==1,
        "production observer joins the original input when actual Present advances the SDK index");
    const auto reads=chain.bufferReads,recorded=provider.recorded;
    Check(invoke(real)==S_OK && calls==before+1 && chain.bufferReads==reads && provider.recorded==recorded,
        "five-second cadence adds no per-frame query, GetBuffer or observer record");
    provider._inputPresentSampleAt=0;
    Check(invoke(real,DXGI_PRESENT_TEST)==S_OK && calls==before+1 && chain.bufferReads==reads,
        "TEST forwards without claiming an input or querying completion");
    provider._inputPresentSampleAt=0;
    auto* original=chain.buffers[chain.index].Get();
    Check(invoke([&] {
        Check(Forwarding::Admit(ComIdentity::Get(&chain),Api::Present1),"one opposite-API SDK forwarding ticket");
        Check(!provider.BeginInputBufferRebuild(),"buffer rebuild cannot replace an active observation epoch");
        return chain.Present(0,0);
    })==S_OK && provider.joined && provider.observedInput==ComIdentity::Get(original),"legitimate SDK forwarding preserves the input association");
    provider._inputPresentSampleAt=0;
    const auto beforeDuplicate=calls;
    Check(invoke([&] {
        Check(Forwarding::Admit(ComIdentity::Get(&chain),Api::Present1),"consume first forwarding ticket");
        Check(!Forwarding::Admit(ComIdentity::Get(&chain),Api::Present1),"duplicate forwarding cannot consume another input");
        return chain.Present(0,0);
    })==S_OK && !provider.joined && calls==beforeDuplicate,"duplicate callback revokes only the query association");
    provider._inputPresentSampleAt=0;
    Check(invoke([&] {++provider.ownerState.lifecycle;return chain.Present(0,0);})==S_OK && !provider.joined && calls==beforeDuplicate,
        "SDK owner change during Present cannot query/publish as the replacement owner");
    provider._inputPresentSampleAt=0;
    Check(invoke([&] {++provider._inputOwnerEpoch;return chain.Present(0,0);})==S_OK && !provider.joined && calls==beforeDuplicate,
        "input owner change during Present cannot inherit a preceding query association");
    provider._inputPresentSampleAt=0;
    chain.bufferResult=E_FAIL;
    Check(invoke(real)==S_OK && calls==beforeDuplicate,"failed GetBuffer observation still forwards original Present once");
    chain.bufferResult=S_OK;
    provider._inputPresentSampleAt=0;
    chain.presentResult=DXGI_ERROR_WAS_STILL_DRAWING;
    Check(invoke(real)==DXGI_ERROR_WAS_STILL_DRAWING && !provider.joined && calls==beforeDuplicate,
        "busy SDK Present retains its HRESULT without consuming completion query");chain.presentResult=S_OK;
    provider._inputPresentSampleAt=0;mode=Mode::Throw;
    Check(invoke(real)==S_OK && !provider.joined && !provider._providerStateQueryActive && calls==beforeDuplicate+1,
        "throwing SDK query preserves successful Present and releases query admission");mode=Mode::LiveFence;
    provider._inputPresentSampleAt=0;
    const auto beforeReentry=calls;
    bool queryCallbackAdmitted=true;
    sdkCallback=[&] {
        queryCallbackAdmitted=Forwarding::Admit(ComIdentity::Get(&chain),Api::Present1);
    };
    Check(invoke(real)==S_OK && !queryCallbackAdmitted && !provider.joined && calls==beforeReentry+1,
        "query reentry revokes outer completion even if the SDK query succeeds");sdkCallback={};
    for (unsigned i=0;i<2;++i) chain.proxies[i].actual=chain.buffers[i];
    chain.useProxy=true;
    provider._inputPresentSampleAt=0;
    bool threw=false;
    try {provider.ObserveInputPresent(&chain,0,Api::Present,[&]()->HRESULT {SetLastError(0xC123);throw std::runtime_error("real Present");});}
    catch (const std::runtime_error&) {threw=true;}
    Check(threw && GetLastError()==0xC123 && !provider._inputPresentDomain.Active() && fence.refs==1 && chain.refs==1 &&
        chain.proxies[0].refs==1 && chain.proxies[1].refs==1,
        "production observer unwinds actual Present exception without leaking admission/references/error");
    provider._inputPresentSampleAt=0;
    Check(invoke(real)==S_OK && provider.joined && chain.proxies[0].refs==1 && chain.proxies[1].refs==1,
        "Release which mutates LastError cannot replace normal SDK Present result/error");
    const auto epoch=provider._inputBufferEpoch;
    Check(provider.BeginInputBufferRebuild() && provider._inputBufferEpoch==epoch+1 && !provider._inputPresentSampleAt,
        "real rebuild attempts advance independent buffer epoch before success/failure is known");
    State::Instance().currentFG=nullptr;liveFence=nullptr;mode=Mode::Full;
}
