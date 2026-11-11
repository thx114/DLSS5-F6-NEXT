// SPDX-License-Identifier: GPL-3.0-or-later
// Actual WARP queues/shared DX11 writes. SDK snapshot = explicit GPU readback.
// No NVIDIA SDK, model inference, swapchain Present or display-FPS measurement.
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;
constexpr UINT BUFFER_COUNT=2;
unsigned checks=0, scenarios=0;
void Check(bool ok,const char* why) {
    ++checks;
    if (!ok) {std::fprintf(stderr,"FAIL: %s\n",why);std::exit(17);}
}
void Hr(HRESULT hr) {Check(SUCCEEDED(hr),"actual D3D operation succeeded");}
void Wait(ID3D12Fence* fence,UINT64 value) {
    HANDLE e=CreateEventW(nullptr,FALSE,FALSE,nullptr);Check(e!=nullptr,"create completion event");
    const auto hr=fence->SetEventOnCompletion(value,e);
    const auto result=SUCCEEDED(hr)?WaitForSingleObject(e,5000):WAIT_FAILED;
    CloseHandle(e);Hr(hr);Check(result==WAIT_OBJECT_0,"bounded real-queue completion");
    Check(fence->GetCompletedValue()!=UINT64_MAX && fence->GetCompletedValue()>=value,"exact fence reached");
}
struct State {
    ID3D12Device* currentD3D12Device=nullptr;
    ID3D12CommandQueue* currentCommandQueue=nullptr;
    static State& Instance(){static State state;return state;}
};
namespace D3D12Hooks {void HookDevice(ID3D12Device*){}}
namespace framegen::final_output {
class Provider {};
thread_local std::shared_ptr<Provider> creating;
std::shared_ptr<Provider> BeginActivity(const std::shared_ptr<Provider>& p){return p;}
void EndActivity(const std::shared_ptr<Provider>&){}
}
bool GenshinConsumerPair(){return false;} // Actual direct-DLSS route has the internal bridge disabled.
bool PublicPairMatches(ID3D12Device*,ID3D12CommandQueue*,D3D12_COMMAND_LIST_TYPE){return true;}
class WithDx12 {
public:
    inline static ID3D12Device* _d3d12Device=nullptr;
    inline static ID3D12CommandQueue* _d3d12CommandQueue=nullptr;
    inline static D3D12_COMMAND_LIST_TYPE _commandListType=D3D12_COMMAND_LIST_TYPE_DIRECT;
    inline static std::mutex _publicationMutex;
    inline static bool _constructingPair=false,_publicationInterrupted=false;
    struct PairReservation {~PairReservation(){_constructingPair=false;}};
    static void UpdateStateObjects(){State::Instance().currentD3D12Device=_d3d12Device;State::Instance().currentCommandQueue=_d3d12CommandQueue;}
    static void AdoptSwapchainQueue(ID3D12CommandQueue*);
    static void BaselineAdopt(ID3D12CommandQueue*);
    static void SetD3D12Objects(ID3D12Device*,ID3D12CommandQueue*,D3D12_COMMAND_LIST_TYPE=D3D12_COMMAND_LIST_TYPE_DIRECT);
    static void BaselineSet(ID3D12Device*,ID3D12CommandQueue*,D3D12_COMMAND_LIST_TYPE=D3D12_COMMAND_LIST_TYPE_DIRECT);
};
class Dx11WithDx12 {
public:
    inline static std::mutex SyncMutex;
    inline static ID3D11DeviceContext4* Dx11DeviceContext=nullptr;
    inline static ID3D11Fence* Dx11FenceTextureCopy=nullptr;
    inline static ID3D12Fence* Dx12FenceTextureCopy=nullptr;
    inline static ID3D12CommandQueue* Dx12CommandQueue=nullptr;
    inline static ID3D11Device5* Dx11Device=nullptr;
    inline static ID3D11Device5* SyncDx11Device=nullptr;
    inline static ID3D12Device* Dx12Device=nullptr;
    inline static ID3D12Device* SyncDx12Device=nullptr;
    inline static ID3D12CommandQueue* SyncDx12CommandQueue=nullptr;
    inline static UINT64 TextureCopyFenceValue=2;
    inline static std::atomic<bool> FgGuideWritesBlocked{false};
    static bool EnsureSyncResourcesLocked(){return Dx11DeviceContext && Dx11FenceTextureCopy && Dx12FenceTextureCopy && Dx12CommandQueue;}
    static void ReleaseSyncResourcesLocked(){} // Fault seam; normal cases retain the real fixture fences.
    static ID3D12CommandQueue* GetD3D12CommandQueue(){return Dx12CommandQueue;}
    static void BlockFgGuideWrites(){FgGuideWritesBlocked.store(true);}
    static bool SyncDx12ToDx11(ID3D12CommandQueue* completedReaderQueue=nullptr);
    static bool MissingWriterWait(ID3D12CommandQueue* completedReaderQueue=nullptr);
};
namespace dlssg::depth_contract {
bool SameOwner(IUnknown* a,IUnknown* b) {
    ComPtr<IUnknown> x,y;
    return a && b && SUCCEEDED(a->QueryInterface(IID_PPV_ARGS(&x))) && SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(&y))) && x==y;
}
}
namespace DlssNr {
unsigned FinishedPictureSubmissionCallbacks(){return 1;}
unsigned FinishedPictureSubmitting(ID3D12CommandQueue*,UINT,ID3D12CommandList**){return 1;}
void FinishedPictureSubmitted(ID3D12CommandQueue*,UINT,ID3D12CommandList**,unsigned){}
}
bool failUiSignal=false,omitWriterWait=false;
HRESULT TestUiSignal(ID3D12CommandQueue* q,ID3D12Fence* f,UINT64 v) {return failUiSignal?E_FAIL:q->Signal(f,v);}
#define LOG_DEBUG(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
struct IFGFeature_Dx12 {bool SubmitUICommandList(UINT){return false;}};
struct DLSSG_Dx12 : IFGFeature_Dx12 {
    std::recursive_mutex _inputContinuityMutex;
    bool _starRailUiFailed=false,_managedRetained=false,quarantined=false,active=true;
    bool _uiCommandListResetted[BUFFER_COUNT]{true,false},_uiCloseFailed[BUFFER_COUNT]{};
    ID3D12CommandQueue* _gameCommandQueue=nullptr;
    ID3D12Fence* _uiFence=nullptr;
    ID3D12GraphicsCommandList* _uiCommandList[BUFFER_COUNT]{};
    UINT64 _uiFenceValue=0,_uiAllocatorFenceValues[BUFFER_COUNT]{};
    bool _dx11GuideBorrowed[BUFFER_COUNT]{true,false};
    UINT64 _dx11GuideSealedFrame[BUFFER_COUNT]{},_frameCount=1;
    unsigned captures=0,observed=0;
    UINT GetIndex(){return 0;}
    bool CanSubmitInputs(){return active && !quarantined;}
    bool IsStarRailUiOwner(){return false;}
    void FailStarRailUiFrame(){active=false;}
    void ObserveGuideSubmission(UINT,ID3D12Fence*,UINT64){++observed;}
    void CaptureAcceptedGuides(UINT){++captures;} // Optional sampler disabled; GPU copy below stands for the SDK snapshot.
    void Deactivate(){active=false;}
    void QuarantineFailedTeardown(){quarantined=true;} // Actual quarantine ownership checked separately in wiring.
    bool SubmitUICommandList(UINT);
    bool SealDx11GuideInputs(bool,ID3D12CommandQueue*);
};
#include "FgHandoffProduction.inl"

namespace {
constexpr UINT Width=11,Height=3;
using Pixel=std::array<float,4>;
struct Warp {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D11Device> device11;
    ComPtr<ID3D11Device5> device5;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11DeviceContext4> context4;
    ComPtr<ID3D12CommandQueue> client,physical;
    ComPtr<ID3D11Fence> ready11;
    ComPtr<ID3D12Fence> ready12,uiDone,gate;
    ComPtr<ID3D11Texture2D> shared;
    std::vector<ComPtr<ID3D11Texture2D>> writes;
    ComPtr<ID3D12Resource> input,readback;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    DLSSG_Dx12 fg;
    Warp() {
        ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter1> adapter;
        Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
        Hr(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
        constexpr D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};
        Hr(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,levels,1,D3D11_SDK_VERSION,&device11,nullptr,&context));
        Hr(device11.As(&device5));Hr(context.As(&context4));
        D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        Hr(device->CreateCommandQueue(&q,IID_PPV_ARGS(&client)));Hr(device->CreateCommandQueue(&q,IID_PPV_ARGS(&physical)));
        Check(!dlssg::depth_contract::SameOwner(client.Get(),physical.Get()),"two real queues, not interface-address aliases");
        Hr(device5->CreateFence(0,D3D11_FENCE_FLAG_SHARED,IID_PPV_ARGS(&ready11)));
        HANDLE handle=nullptr;Hr(ready11->CreateSharedHandle(nullptr,GENERIC_ALL,nullptr,&handle));
        Hr(device->OpenSharedHandle(handle,IID_PPV_ARGS(&ready12)));CloseHandle(handle);
        Hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&uiDone)));
        Hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)));
        D3D11_TEXTURE2D_DESC d{};d.Width=Width;d.Height=Height;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
        d.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        d.MiscFlags=D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        Hr(device11->CreateTexture2D(&d,nullptr,&shared));ComPtr<IDXGIResource1> exported;Hr(shared.As(&exported));
        Hr(exported->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&handle));
        Hr(device->OpenSharedHandle(handle,IID_PPV_ARGS(&input)));CloseHandle(handle);
        auto desc=input->GetDesc();UINT64 bytes=0;device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&bytes);
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=bytes;
        buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)));
        Hr(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&allocator)));
        Hr(device->CreateCommandList(0,q.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition={input.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE};
        list->ResourceBarrier(1,&barrier);
        D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=input.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource=readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=footprint;
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);list->ResourceBarrier(1,&barrier);
        fg._gameCommandQueue=client.Get();fg._uiFence=uiDone.Get();fg._uiCommandList[0]=list.Get();
        Dx11WithDx12::Dx11DeviceContext=context4.Get();Dx11WithDx12::Dx11FenceTextureCopy=ready11.Get();
        Dx11WithDx12::Dx12FenceTextureCopy=ready12.Get();Dx11WithDx12::Dx12CommandQueue=client.Get();
        Dx11WithDx12::Dx11Device=Dx11WithDx12::SyncDx11Device=device5.Get();
        Dx11WithDx12::Dx12Device=Dx11WithDx12::SyncDx12Device=device.Get();
        Dx11WithDx12::SyncDx12CommandQueue=client.Get();
        Dx11WithDx12::TextureCopyFenceValue=2;Dx11WithDx12::FgGuideWritesBlocked=false;
        State::Instance()={device.Get(),client.Get()};WithDx12::_d3d12Device=device.Get();WithDx12::_d3d12CommandQueue=client.Get();
    }
    ~Warp() {
        // Fixture cleanup only. Never used as production retirement proof.
        Hr(gate->Signal(1));context->Flush();ComPtr<ID3D12Fence> f;
        Hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f)));Hr(client->Signal(f.Get(),1));Wait(f.Get(),1);
        State::Instance()={};WithDx12::_d3d12Device=nullptr;WithDx12::_d3d12CommandQueue=nullptr;
    }
    void Write(unsigned frame) {
        std::array<Pixel,Width*Height> pixels{};
        for(UINT y=0;y<Height;++y)for(UINT x=0;x<Width;++x)pixels[y*Width+x]={float(frame),float(x),float(y),1};
        // WARP can perform UpdateSubresource directly on CPU-addressable shared
        // memory. Use a real queued GPU copy so the writer obeys DX11 Wait.
        D3D11_TEXTURE2D_DESC desc{};shared->GetDesc(&desc);desc.MiscFlags=0;
        D3D11_SUBRESOURCE_DATA initial{pixels.data(),Width*sizeof(Pixel),0};
        ComPtr<ID3D11Texture2D> upload;Hr(device11->CreateTexture2D(&desc,&initial,&upload));
        context->CopyResource(shared.Get(),upload.Get());writes.push_back(std::move(upload));
    }
    void Expect(unsigned frame) {
        void* data=nullptr;Hr(readback->Map(0,nullptr,&data));
        for(UINT y=0;y<Height;++y)for(UINT x=0;x<Width;++x){
            Pixel p{};std::memcpy(p.data(),static_cast<unsigned char*>(data)+footprint.Offset+size_t(y)*footprint.Footprint.RowPitch+x*sizeof(Pixel),sizeof(p));
            if(p[0]!=float(frame))std::fprintf(stderr,"pixel expected source %u, actual %.0f\n",frame,p[0]);
            Check(p==Pixel{float(frame),float(x),float(y),1},"GPU snapshot uses the matching source frame at every pixel");
        }
        D3D12_RANGE noWrites{0,0};readback->Unmap(0,&noWrites);
    }
};
void QueueIdentity(bool baseline=false) {
    Warp w;++scenarios;
    const auto adopt=[&](ID3D12CommandQueue* q){if(baseline)WithDx12::BaselineAdopt(q);else WithDx12::AdoptSwapchainQueue(q);};
    adopt(w.client.Get());
    {
        // Even absent optional endpoint admission, SDK creation is not a source.
        framegen::final_output::CreationScope sdk(nullptr);
        Check(framegen::final_output::CreationScope::Active(),"SDK creation role survives unavailable optional endpoint");
        adopt(w.physical.Get());
        Check(State::Instance().currentCommandQueue==w.client.Get() && WithDx12::_d3d12CommandQueue==w.client.Get(),"SDK physical factory cannot overwrite source queue");
        {framegen::final_output::CreationScope nested(nullptr);adopt(w.physical.Get());}
        Check(framegen::final_output::CreationScope::Active(),"nested factory restores outer SDK role");
    }
    Check(!framegen::final_output::CreationScope::Active(),"creation scope restores source role");
}
void Pixels(bool early,bool delayedRead,bool missingWait=false) {
    Warp w;++scenarios;omitWriterWait=missingWait;
    w.Write(1);Hr(w.context4->Signal(w.ready11.Get(),1));w.context->Flush();Hr(w.client->Wait(w.ready12.Get(),1));
    if(delayedRead || !early)Hr(w.client->Wait(w.gate.Get(),1));
    if(early) {
        Check(w.fg.SealDx11GuideInputs(true,w.client.Get()),"actual early guide seal succeeds");
        Check(w.fg._dx11GuideSealedFrame[0]==1 && !w.fg._uiCommandListResetted[0],"sealed recording is submitted once");
    } else Check(w.fg.SubmitUICommandList(0),"legacy delayed guide recording submits");
    if(early && !delayedRead)Hr(w.client->Wait(w.gate.Get(),1)); // Stand-in for long presentation NR/FG after the snapshot.
    w.Write(2);const auto next=Dx11WithDx12::TextureCopyFenceValue++;
    Hr(w.context4->Signal(w.ready11.Get(),next));w.context->Flush();
    if(early && delayedRead && !missingWait) {
        Check(w.uiDone->GetCompletedValue()<1,"guide read is still GPU delayed after CPU seal returned");
        HANDLE e=CreateEventW(nullptr,FALSE,FALSE,nullptr);Check(e!=nullptr,"writer pending event");
        Hr(w.ready12->SetEventOnCompletion(next,e));
        Check(WaitForSingleObject(e,30)==WAIT_TIMEOUT,"next DX11 write waits for actual guide read");CloseHandle(e);
    } else {
        Wait(w.ready12.Get(),next);
        Check(w.gate->GetCompletedValue()==0,"next DX11 writer completes without waiting for presentation NR/FG");
    }
    Hr(w.gate->Signal(1));Wait(w.uiDone.Get(),1);if(early && !missingWait)Wait(w.ready12.Get(),next);
    w.Expect(1);omitWriterWait=false;
}
void Failures() {
    for(unsigned fault=0;fault<9;++fault){Warp w;++scenarios;failUiSignal=fault==4;
        if(fault==1)w.fg.active=false;if(fault==2)w.fg._uiCloseFailed[0]=true;
        if(fault==5)Dx11WithDx12::SyncDx12CommandQueue=w.physical.Get();
        if(fault==6)Dx11WithDx12::Dx11FenceTextureCopy=nullptr;
        if(fault==7)Dx11WithDx12::TextureCopyFenceValue=UINT64_MAX-1;
        if(fault==8)Hr(w.list->Close()); // Actual Close failure, not a simulated state bit.
        const bool ok=w.fg.SealDx11GuideInputs(fault!=0,fault==3?w.physical.Get():w.client.Get());
        Check(!ok && Dx11WithDx12::FgGuideWritesBlocked.load() && w.fg.quarantined,"uncertain or mismatched borrowed read blocks future writes and retains owners");
        Check(w.fg._dx11GuideBorrowed[0] && w.fg._dx11GuideSealedFrame[0]==0,"failure cannot publish a sealed source");
        if(fault==4)Check(!w.fg._uiCommandListResetted[0],"post-Execute Signal failure cannot replay recording");
        failUiSignal=false;
    }
}
void SourceValidationAndQueueChange() {
    {Warp w;++scenarios;w.fg._dx11GuideBorrowed[0]=false;
        Check(!w.fg.SealDx11GuideInputs(false,w.client.Get()),"pre-record validation or warmup cannot publish successful tags");
        Check(!Dx11WithDx12::FgGuideWritesBlocked.load() && !w.fg.quarantined && w.fg._uiCommandListResetted[0],
            "pre-record refusal leaves shared uploads and safe rebuild available");}
    {Warp w;++scenarios;
        // No mutable State/provider re-resolution after the reader Execute.
        Dx11WithDx12::Dx12CommandQueue=w.physical.Get();
        Check(w.fg.SealDx11GuideInputs(true,w.client.Get())==false,"pre-seal producer mismatch is refused");}
    {Warp w;++scenarios;
        State::Instance().currentCommandQueue=w.physical.Get();WithDx12::_d3d12CommandQueue=w.physical.Get();
        Check(Dx11WithDx12::SyncDx12ToDx11(w.client.Get()),"exact reader handback does not adopt mutable global physical queue");
        Wait(w.ready12.Get(),2);
        Check(!Dx11WithDx12::FgGuideWritesBlocked.load(),"valid existing source fence domain stays usable");}
}
}
int main(int argc,char** argv) {
    const auto mode=argc>1?argv[1]:"";
    if(std::strcmp(mode,"--negative-queue")==0)QueueIdentity(true);
    else if(std::strcmp(mode,"--negative-guides")==0)Pixels(false,true);
    else if(std::strcmp(mode,"--negative-handback")==0)Pixels(true,true,true);
    else {QueueIdentity();Pixels(true,true);Pixels(true,false);Failures();SourceValidationAndQueueChange();}
    std::printf("PASS %u scenarios, %u checks; actual WARP queue/pixel handoff; SDK/game/FPS untested\n",scenarios,checks);
}
