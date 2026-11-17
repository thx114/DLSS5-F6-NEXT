// SPDX-License-Identifier: GPL-3.0-or-later
// Standalone translation unit: compile THIS file, not an additional GuideCapture.cpp.
// Including production once exposes read-only lifecycle observations and the
// existing GUIDE_PREVIEW_TESTING Signal fault, without a duplicate state machine.
// Requires the same generated GuideCaptureHooksProduction.inl as capture WARP.
#ifndef GUIDE_PREVIEW_TESTING
#define GUIDE_PREVIEW_TESTING
#endif
#include "../GuideCapture.h"
// Instrument only the diagnostic clock boundary of the included production
// sampler. Per-thread counting excludes its asynchronous retirement worker;
// all demand, admission and recording decisions remain production code.
namespace GuideClockFixture {
using namespace GuideDiagnostics;
inline thread_local uint64_t reads=0,overrideTick=0;
inline uint64_t Now() noexcept {
    ++reads;return overrideTick?overrideTick:GuideDiagnostics::Now();
}
}
#define GuideDiagnostics GuideClockFixture
#include "../GuideCapture.cpp"
#undef GuideDiagnostics
#include <dxgi1_6.h>
#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <tuple>
#include <thread>
#include "PreviewDeviceAlias.h"
#include "../../shaders/dlssnr/DlssNr_FrameInfo.h"

namespace HookFixture {
namespace Util {inline bool CheckForRealObject(const char*,IUnknown*,IUnknown**){return false;}}
namespace DetailReuse {inline ID3D12CommandList* RealSubmittedCommands(ID3D12CommandList* p){return p;}}
inline uint64_t finishedPictureSubmitCallbacks=0;
inline void FinishedPictureSubmittedReal(ID3D12CommandQueue*,UINT,ID3D12CommandList* const*){}
#include "GuideCaptureHooksProduction.inl"
}

namespace OwnerFixture {
namespace DlssNr {
void FinishedPictureResetCommandList(ID3D12CommandList* list){
    GuideCapture::Reset(static_cast<ID3D12GraphicsCommandList*>(list));
}
}
// Execute the actual provider cleanup and its actual UI owner-fence poll.
// This adapter supplies WARP owners; it does not implement retirement policy.
struct DLSSG_Dx12 {
    static constexpr unsigned BUFFER_COUNT=4;
    enum class UIAllocatorStatus {Invalid,Busy,Ready};
    ID3D12Device* _device=nullptr;
    ID3D12CommandQueue* _gameCommandQueue=nullptr;
    ID3D12Fence* _uiFence=nullptr;
    HANDLE _uiFenceEvent=nullptr;
    ID3D12CommandAllocator* _uiCommandAllocator[BUFFER_COUNT]{};
    ID3D12GraphicsCommandList* _uiCommandList[BUFFER_COUNT]{};
    bool _uiCommandListResetted[BUFFER_COUNT]{},_uiCloseFailed[BUFFER_COUNT]{};
    UINT64 _uiFenceValue=0,_uiAllocatorFenceValues[BUFFER_COUNT]{};
    UIAllocatorStatus PollUIAllocator(UINT) const;
    void ResetCompletedGuideRecordings() noexcept;
};
#include "GuidePreviewOwnerProduction.inl"
}

using Microsoft::WRL::ComPtr;
using GuideCapture::Stream;
using GuideCapture::PreviewMode;
using GuideCapture::PreviewSnapshot;
using GuideCapture::ExecutionReceipt;
static_assert(std::tuple_size<decltype(ExecutionReceipt{}.preview)>::value==27);
static_assert(std::tuple_size<decltype(ExecutionReceipt{}.offline)>::value==12);
static unsigned checks=0;
static void Check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
static void Hr(HRESULT hr){Check(SUCCEEDED(hr),"real WARP D3D call failed");}
template<class F> static void Until(F&& ready,const char* why){
    const auto end=GetTickCount64()+10000;
    do{if(ready())return;Sleep(2);}while(GetTickCount64()<end);
    throw std::runtime_error(std::string(why)+": "+GuideCapture::PreviewStatus());
}
static void NeverAcquire(const char* why){
    const auto end=GetTickCount64()+40;
    do{PreviewSnapshot s;Check(!GuideCapture::TryAcquireCompleted(s),why);Sleep(2);}while(GetTickCount64()<end);
}
static GuideDiagnostics::Snapshot Stamp(uint64_t source){
    GuideDiagnostics::Snapshot s;s.source=source;s.epoch=source+1000;
    s.feature=101;s.configuration=202;s.owner=303;s.tick=GuideDiagnostics::Now()-40;
    s.modelSuccess=false;return s; // FG diagnostics must not need NR/NGX.
}
static void Demand(PreviewMode mode,float gain=3,bool flip=false,bool raw=false,bool frozen=false){
    GuideCapture::PreviewDemand d;d.mode=mode;d.gain=gain;d.flipY=flip;d.raw=raw;d.frozen=frozen;
    GuideCapture::SetPreviewDemand(d);
}
static void TestOpenFgGuideDemand(){
    GuideCapture::Request(false);GuideCapture::RequestHistoryMeasurement(false);
    struct Case {PreviewMode mode;bool open;};
    const Case cases[]{
        {PreviewMode::Off,false},{PreviewMode::NrOriginal,false},
        {PreviewMode::NrColour,false},{PreviewMode::NrDepth,false},
        {PreviewMode::NrMotion,false},{PreviewMode::FgColour,false},
        {PreviewMode::FgDepth,true},{PreviewMode::FgMotion,true},
        {PreviewMode::History,false}
    };
    for(const auto& c:cases)for(bool frozen:{false,true}){
        Demand(c.mode,3,false,false,frozen);
        const auto before=GuideClockFixture::reads;
        Check(GuideCapture::NeedsOpenFgGuideRecording()==c.open,
              "only selected FG guides retain the source recording, including while frozen");
        Check(GuideCapture::GetPreviewMode()==c.mode,"atomic routing preserves the actual selected mode");
        Check(GuideClockFixture::reads==before,"preview-only guide demand needs no diagnostic clock");
        if(frozen){GuideCapture::SampleToken token;
            Check(!GuideCapture::TryBeginSample(Stream::Fg,token),"frozen guide demand cannot reserve new preview work");}
    }
    Demand(PreviewMode::Off);
    const auto before=GuideClockFixture::reads;
    for(unsigned i=0;i<32;++i){
        for(Stream stream:{Stream::Source,Stream::NrModel,Stream::Fg,Stream::History,Stream::Count,
                           static_cast<Stream>(UINT_MAX)}){
            Check(!GuideCapture::OfflineWanted(stream),"disabled and invalid streams have no offline demand");
            Check(!GuideCapture::OfflineSampleDue(stream),"disabled and invalid streams admit no offline sample");
        }
        Check(!GuideCapture::NeedsOpenFgGuideRecording(),"normal operation permits early guide sealing");
        GuideCapture::SampleToken token;
        Check(!GuideCapture::TryBeginSample(Stream::Fg,token),"Off refuses sampling before clock access");
    }
    Check(GuideClockFixture::reads==before,"normal no-demand helper and sampling paths make zero clock calls");

    // Another lane's live deadline must not add FG clock work or hold guides.
    GuideCapture::Request(Stream::History,true,true);
    GuideCapture::RequestHistoryMeasurement();
    Check(GuideCapture::OfflineWanted(Stream::History)&&GuideCapture::OfflineWanted(Stream::Source),
          "history and calibration requests remain live on their own streams");
    const auto unrelated=GuideClockFixture::reads;
    Check(!GuideCapture::OfflineWanted(Stream::Fg)&&!GuideCapture::OfflineSampleDue(Stream::Fg)&&
          !GuideCapture::NeedsOpenFgGuideRecording(),"unrelated offline demand permits early FG sealing");
    Check(GuideClockFixture::reads==unrelated,"unrelated live deadlines make zero FG clock calls");
    GuideCapture::Request(false);GuideCapture::RequestHistoryMeasurement(false);

    // Colour preview may freeze, but an independent offline request still
    // needs the original guides. Advancing only this thread's clock exercises
    // real deadline handling without sleeps or changes to production state.
    Demand(PreviewMode::FgColour,3,false,false,true);
    GuideCapture::Request(Stream::Fg,true);
    const auto active=GuideClockFixture::reads;
    Check(GuideCapture::OfflineWanted(Stream::Fg)&&GuideCapture::NeedsOpenFgGuideRecording(),
          "offline FG request retains guides independently of frozen colour preview");
    Check(GuideClockFixture::reads>active,"active offline demand actually consults the instrumented clock");
    GuideClockFixture::overrideTick=GuideDiagnostics::Now()+2000;
    Check(!GuideCapture::OfflineWanted(Stream::Fg)&&!GuideCapture::OfflineSampleDue(Stream::Fg)&&
          !GuideCapture::NeedsOpenFgGuideRecording(),"expired nonzero FG deadline cannot retain guide recording");
    GuideCapture::Request(false);GuideClockFixture::overrideTick=0;
    // A visible menu/worker may hold demand.mutex. Routing must remain an
    // exact atomic observation, not wait for that lock or pretend it is Off.
    HANDLE observedEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    Check(observedEvent!=nullptr,"preview routing completion event");
    PreviewMode observedMode=PreviewMode::Off;bool observedActive=false;
    std::unique_lock demandLock(GuideCapture::PreviewDetail::Get().mutex);
    std::thread routingReader([&]{observedMode=GuideCapture::GetPreviewMode();
        observedActive=GuideCapture::PreviewActive();SetEvent(observedEvent);});
    const auto observedBeforeUnlock=WaitForSingleObject(observedEvent,1000);
    demandLock.unlock();routingReader.join();CloseHandle(observedEvent);
    Check(observedBeforeUnlock==WAIT_OBJECT_0&&observedMode==PreviewMode::FgColour&&observedActive,
          "normal rendering mode query completes while the diagnostic demand mutex stays locked");
    Demand(PreviewMode::Off);
}
struct Recording {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    std::vector<ComPtr<ID3D12CommandAllocator>> oldAllocators;
    ExecutionReceipt receipt;bool closed=false;
};
struct Observation {
    bool found=false,active=false,pre=false,post=false,reset=false,done=false,quarantined=false,signalled=false,published=false,inputsHeld=false;
    unsigned readers=0,readersDone=0,readersReset=0,readersQuarantined=0;
    uint64_t generation=0;
};
static Observation Observe(uint64_t source){
    auto& state=GuideCapture::PreviewDetail::Get();std::lock_guard lock(state.mutex);
    Observation o;
    for(const auto& slot:state.slots)if(slot.source==source){
        const auto& p=slot.producer;o.found=true;o.active=p.Active();o.pre=p.pre;o.post=p.post;o.reset=p.reset;
        o.done=p.done;o.quarantined=p.quarantined;o.signalled=p.signalled;o.published=slot.published;
        o.inputsHeld=slot.surface&&slot.surface->inputs[0];o.generation=p.generation;
        for(const auto& r:slot.readers)if(r.Active()){
            ++o.readers;o.readersDone+=r.done;o.readersReset+=r.reset;o.readersQuarantined+=r.quarantined;
        }break;
    }return o;
}
static PreviewSnapshot Acquire(uint64_t source){
    PreviewSnapshot out;
    Until([&]{return GuideCapture::TryAcquireCompleted(out)&&out.source==source;},"completed immutable GPU snapshot");return out;
}
static GuideCapture::PreviewProgress Progress(){
    GuideCapture::PreviewProgress progress;
    Until([&]{return GuideCapture::ReadPreviewProgress(progress);},"nonblocking preview progress observation");
    return progress;
}
static GuideCapture::PreviewSlotProgress ProgressSlot(uint64_t source){
    const auto progress=Progress();
    for(const auto& slot:progress.slots)if(slot.source==source)return slot;
    throw std::runtime_error("preview progress did not retain the actual source identity");
}
struct Draw {
    Recording commands;
    ComPtr<ID3D12Resource> target,readback;
    ComPtr<ID3D12DescriptorHeap> rtv;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    unsigned width=0,height=0;
};
struct Fixture {
    static constexpr unsigned Width=337,Height=211;
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> producer,reader;
    std::vector<ComPtr<ID3D12Resource>> uploads;
    std::array<ComPtr<ID3D12Resource>,3> inputs;
    Fixture(){
        ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter> warp;
        Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
        Hr(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
        D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        Hr(device->CreateCommandQueue(&q,IID_PPV_ARGS(&producer)));Hr(device->CreateCommandQueue(&q,IID_PPV_ARGS(&reader)));
        auto upload=New();
        inputs[0]=Texture(upload,DXGI_FORMAT_R32G32B32A32_FLOAT,16,[](unsigned x,unsigned y,void* p){
            const float v[]{float(x)/512,float(y)/256,.25f,1};std::memcpy(p,v,sizeof(v));});
        inputs[1]=Texture(upload,DXGI_FORMAT_R32_FLOAT,4,[](unsigned,unsigned y,void* p){
            const float z=float(y)/256;std::memcpy(p,&z,4);});
        inputs[2]=Texture(upload,DXGI_FORMAT_R32G32_FLOAT,8,[](unsigned x,unsigned y,void* p){
            const float v[]{float(int(x%17)-8)*.0025f,float(int(y%13)-6)*.00375f};std::memcpy(p,v,sizeof(v));});
        Execute(upload,producer.Get());Drain(producer.Get());Reset(upload);Close(upload);
    }
    Recording New(){
        Recording r;Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&r.allocator)));
        Hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,r.allocator.Get(),nullptr,IID_PPV_ARGS(&r.list)));return r;
    }
    static void Close(Recording& r){Check(!r.closed,"Close exactly once per actual recording");Hr(r.list->Close());r.closed=true;}
    void Reset(Recording& r){
        Check(r.closed,"Reset requires actually closed list");ComPtr<ID3D12CommandAllocator> next;
        Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&next)));
        Hr(r.list->Reset(next.Get(),nullptr));r.oldAllocators.push_back(std::move(r.allocator));r.allocator=std::move(next);
        r.closed=false;GuideCapture::Reset(r.list.Get()); // after real successful Reset, never before.
    }
    static void Post(Recording& r,ID3D12CommandQueue* queue,const ExecutionReceipt& receipt){
        ID3D12CommandList* lists[]{r.list.Get()};HookFixture::FinishedPictureSubmitted(queue,1,lists,receipt);
    }
    void Execute(Recording& r,ID3D12CommandQueue* queue,bool post=true,bool close=true){
        if(close)Close(r);Check(r.closed,"Execute requires closed recording");ID3D12CommandList* lists[]{r.list.Get()};
        r.receipt=HookFixture::FinishedPictureSubmitting(queue,1,lists);
        const auto nested=HookFixture::FinishedPictureSubmitting(queue,1,lists);
        Check(r.receipt.preview==nested.preview&&r.receipt.offline==nested.offline,"duplicate nested pre is generation-idempotent");
        queue->ExecuteCommandLists(1,lists);if(post)Post(r,queue,r.receipt);
    }
    void Drain(ID3D12CommandQueue* queue){
        ComPtr<ID3D12Fence> fence;Hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));Hr(queue->Signal(fence.Get(),1));
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);Check(event!=nullptr,"test fence event");
        const auto hr=fence->SetEventOnCompletion(1,event);const auto wait=SUCCEEDED(hr)?WaitForSingleObject(event,10000):WAIT_FAILED;
        CloseHandle(event);Hr(hr);Check(wait==WAIT_OBJECT_0&&fence->GetCompletedValue()>=1,"actual selected WARP queue fence completed");
    }
    ComPtr<ID3D12Resource> Texture(Recording& r,DXGI_FORMAT format,unsigned bytesPerPixel,const std::function<void(unsigned,unsigned,void*)>& fill){
        D3D12_RESOURCE_DESC td{};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;td.Width=Width;td.Height=Height;
        td.DepthOrArraySize=1;td.MipLevels=1;td.Format=format;td.SampleDesc.Count=1;
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;ComPtr<ID3D12Resource> texture,upload;
        Hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&texture)));
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes=0;device->GetCopyableFootprints(&td,0,1,0,&fp,nullptr,nullptr,&bytes);
        D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=bytes;bd.Height=1;bd.DepthOrArraySize=1;
        bd.MipLevels=1;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;hp.Type=D3D12_HEAP_TYPE_UPLOAD;
        Hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload)));
        void* mapped=nullptr;Hr(upload->Map(0,nullptr,&mapped));
        for(unsigned y=0;y<Height;++y)for(unsigned x=0;x<Width;++x)
            fill(x,y,static_cast<unsigned char*>(mapped)+fp.Offset+size_t(y)*fp.Footprint.RowPitch+x*bytesPerPixel);
        upload->Unmap(0,nullptr);D3D12_TEXTURE_COPY_LOCATION from{},to{};
        from.pResource=upload.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=fp;
        to.pResource=texture.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;r.list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        GuideCapture::Barrier(r.list.Get(),texture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        uploads.push_back(upload);return texture;
    }
    GuideCapture::Inputs Inputs() const {
        GuideCapture::Inputs in;in.colour=inputs[0].Get();in.depth=inputs[1].Get();in.motion=inputs[2].Get();
        in.colourRect=in.depthRect=in.motionRect={7,5,321,201};in.motionToUvX=-.75f;in.motionToUvY=1.25f;
        in.independentRecording=true;return in;
    }
    void Record(Recording& r,const GuideCapture::Inputs& in,const GuideDiagnostics::Snapshot& stamp,Stream stream=Stream::Fg){
        Until([&]{GuideCapture::SampleToken token;return GuideCapture::TryBeginSample(stream,token)&&
            GuideCapture::TryRecord(token,r.list.Get(),in,stamp);},"actual supplied FG resource recording");
    }
    void Record(Recording& r,uint64_t source,const GuideCapture::Inputs& in,Stream stream=Stream::Fg){Record(r,in,Stamp(source),stream);}
    PreviewSnapshot Complete(uint64_t source,const GuideCapture::Inputs& in,Stream stream=Stream::Fg){
        auto r=New();Record(r,source,in,stream);Execute(r,producer.Get());Drain(producer.Get());
        Until([&]{return Observe(source).done;},"actual producer fence, still executable");NeverAcquire("producer fence alone is insufficient without successful Reset");
        Reset(r);auto result=Acquire(source);Close(r);return result;
    }
    Draw MakeDraw(const PreviewSnapshot& snapshot,bool copy=true,unsigned width=0,unsigned height=0){
        Draw d;d.commands=New();d.width=width?width:snapshot.width;d.height=height?height:snapshot.height;
        D3D12_RESOURCE_DESC td{};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;td.Width=d.width;td.Height=d.height;
        td.DepthOrArraySize=1;td.MipLevels=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.SampleDesc.Count=1;td.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        Hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&d.target)));
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=1;
        Hr(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&d.rtv)));const auto handle=d.rtv->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(d.target.Get(),nullptr,handle);d.commands.list->OMSetRenderTargets(1,&handle,FALSE,nullptr);
        Until([&]{return GuideCapture::DrawPreview(snapshot,d.commands.list.Get(),td.Format,d.width,d.height);},
            "actual production pixel shader records selected snapshot on the output device");
        if(copy){
            UINT64 bytes=0;device->GetCopyableFootprints(&td,0,1,0,&d.footprint,nullptr,nullptr,&bytes);
            D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=bytes;bd.Height=1;bd.DepthOrArraySize=1;
            bd.MipLevels=1;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;hp.Type=D3D12_HEAP_TYPE_READBACK;
            Hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&d.readback)));
            GuideCapture::Barrier(d.commands.list.Get(),d.target.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=d.target.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            to.pResource=d.readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=d.footprint;
            d.commands.list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        }return d;
    }
    std::vector<std::array<unsigned char,4>> Pixels(const PreviewSnapshot& snapshot,unsigned width=0,unsigned height=0){
        auto d=MakeDraw(snapshot,true,width,height);Execute(d.commands,reader.Get());Drain(reader.Get());Reset(d.commands);
        // Map ONLY our render-target copy, after its actual reader queue fence.
        void* data=nullptr;const D3D12_RANGE range{0,size_t(d.readback->GetDesc().Width)};
        Hr(d.readback->Map(0,&range,&data));std::vector<std::array<unsigned char,4>> pixels(size_t(d.width)*d.height);
        for(unsigned y=0;y<d.height;++y)std::memcpy(pixels.data()+size_t(y)*d.width,
            static_cast<unsigned char*>(data)+d.footprint.Offset+size_t(y)*d.footprint.Footprint.RowPitch,size_t(d.width)*4);
        const D3D12_RANGE written{0,0};d.readback->Unmap(0,&written);Close(d.commands);return pixels;
    }
};
struct QueueGate {
    ComPtr<ID3D12Fence> fence;bool released=false;
    QueueGate(Fixture& f,ID3D12CommandQueue* queue){Hr(f.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));Hr(queue->Wait(fence.Get(),1));}
    void Release(){Hr(fence->Signal(1));released=true;}
    ~QueueGate(){if(!released&&fence)fence->Signal(1);}
};
static void TestCallerPreviewAuthority(Fixture& fixture){
    const auto authorized=[](ID3D12GraphicsCommandList* cmdList,ID3D12GraphicsCommandList* owner){
        struct Source {ID3D12GraphicsCommandList* ownedPreviewRecording;};
        const Source source{owner};DlssNrFrameInfo frame;
        frame.SourceOutput=true;
#include "GuidePreviewAuthorityProduction.inl"
        Check(!frame.IndependentCommands&&!frame.FinishedPicture,"preview authority never grants independent model ownership");
        return frame.CanRecordPreview();
    };
    auto current=fixture.New(),other=fixture.New();
    Check(authorized(current.list.Get(),current.list.Get()),"this actual caller-owned DIRECT list can record previews");
    Check(!authorized(current.list.Get(),other.list.Get())&&!authorized(other.list.Get(),current.list.Get()),
        "different and previous-call DIRECT lists cannot authorize this preview");
    Check(!authorized(nullptr,nullptr)&&!authorized(current.list.Get(),nullptr),"null ownership never authorizes preview commands");
    Fixture::Close(current);Fixture::Close(other);
    for(const auto kind:{D3D12_COMMAND_LIST_TYPE_COMPUTE,D3D12_COMMAND_LIST_TYPE_COPY}){
        ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> commands;
        Hr(fixture.device->CreateCommandAllocator(kind,IID_PPV_ARGS(&allocator)));
        Hr(fixture.device->CreateCommandList(0,kind,allocator.Get(),nullptr,IID_PPV_ARGS(&commands)));
        Check(!authorized(commands.Get(),commands.Get()),"actual COMPUTE/COPY ownership cannot authorize DIRECT-only preview draws");
        Hr(commands->Close());
    }
}
static void TestCompletedOwnerRing(Fixture& f){
    Demand(PreviewMode::FgColour);GuideCapture::Request(false);
    OwnerFixture::DLSSG_Dx12 owner;
    owner._device=f.device.Get();owner._gameCommandQueue=f.producer.Get();
    ComPtr<ID3D12Fence> completed;
    Hr(f.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&completed)));
    owner._uiFence=completed.Get();owner._uiFenceEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    Check(owner._uiFenceEvent!=nullptr,"real UI owner event");
    std::array<Recording,4> ring;
    for(unsigned i=0;i<ring.size();++i){
        ring[i]=f.New();owner._uiCommandList[i]=ring[i].list.Get();
        owner._uiCommandAllocator[i]=ring[i].allocator.Get();owner._uiCommandListResetted[i]=true;
    }
    QueueGate gate(f,f.producer.Get());
    for(unsigned i=0;i<3;++i){
        f.Record(ring[i],501+i,f.Inputs());
        owner.ResetCompletedGuideRecordings();
        Check(!Observe(501+i).reset,"open sampled recording is not discarded by cleanup");
        f.Execute(ring[i],f.producer.Get());owner._uiCommandListResetted[i]=false;
        owner._uiAllocatorFenceValues[i]=++owner._uiFenceValue;
        Hr(f.producer->Signal(completed.Get(),owner._uiAllocatorFenceValues[i]));
    }
    GuideCapture::SampleToken full;
    Check(!GuideCapture::TryBeginSample(Stream::Fg,full),"three sampled slots cannot span a four-source Reset delay");
    owner.ResetCompletedGuideRecordings();
    for(unsigned i=0;i<3;++i)
        Check(!Observe(501+i).reset,"busy actual UI owner fence forbids early Reset");
    gate.Release();f.Drain(f.producer.Get());
    // Completion by itself leaves each executable recording and its snapshot
    // pinned. The actual cleanup must Reset, without waiting for ring wrap.
    Until([&]{owner.ResetCompletedGuideRecordings();
        return Observe(501).reset&&Observe(502).reset&&Observe(503).reset;},"actual completed UI owner recordings Reset");
    for(unsigned i=0;i<3;++i){
        Check(Observe(501+i).reset,"completed sampled recording actually Reset");
        Check(!owner._uiCommandListResetted[i]&&!owner._uiCloseFailed[i],"cleanup leaves closed unreserved owner recording");
        Check(owner._uiAllocatorFenceValues[i]==i+1,"cleanup preserves actual submitted target");
    }
    Check(owner._uiFenceValue==3&&owner._uiCommandListResetted[3],"cleanup reserves no unsignalled target and leaves open unsampled list intact");
    auto latest=Acquire(503);latest={};
    f.Record(ring[3],504,f.Inputs());f.Execute(ring[3],f.producer.Get());owner._uiCommandListResetted[3]=false;
    owner._uiAllocatorFenceValues[3]=++owner._uiFenceValue;Hr(f.producer->Signal(completed.Get(),owner._uiFenceValue));
    f.Drain(f.producer.Get());Until([&]{owner.ResetCompletedGuideRecordings();return Observe(504).reset;},"fourth completed UI owner recording Reset");
    latest=Acquire(504);latest={};
    CloseHandle(owner._uiFenceEvent);Demand(PreviewMode::Off);
}
static void PixelNear(const std::array<unsigned char,4>& actual,float r,float g,float b,const char* why){
    const float wanted[]{r,g,b,1};for(unsigned i=0;i<4;++i)
        Check(std::abs(float(actual[i])-255.f*(std::clamp)(wanted[i],0.f,1.f))<=1.1f,why);
}
static void TestPixelsAndDemand(Fixture& f){
    Check(!GuideCapture::PreviewActive()&&GuideCapture::GetPreviewDemand().mode==PreviewMode::Off,"default mode is Off");
    auto off=f.New();auto in=f.Inputs();
    for(unsigned i=0;i<20;++i){GuideCapture::SampleToken token;
        Check(!GuideCapture::TryBeginSample(Stream::Fg,token),"Off grants no sampling reservation");
        Check(!GuideCapture::Record(Stream::Fg,off.list.Get(),in,Stamp(1)),"default Off records zero GPU work");}
    {auto& state=GuideCapture::PreviewDetail::Get();std::lock_guard lock(state.mutex);
        Check(state.serial==0&&state.generation==0,"Off creates no recording generations");
        for(const auto& slot:state.slots)Check(!slot.surface&&!slot.requestedDevice&&!slot.reserved,"Off schedules no GPU preparation");}
    Fixture::Close(off);Demand(PreviewMode::FgColour);GuideCapture::Request(false);
    Sleep(1050);Check(GuideCapture::PreviewWanted(Stream::Fg),"GPU demand persists independently of menu/offline request expiry");
    Check(!GuideCapture::Wanted(Stream::NrModel)&&!GuideCapture::Wanted(Stream::Source)&&!GuideCapture::Wanted(Stream::History),"NR-off FG-only demand");
    auto stamp=Stamp(10);auto r=f.New();
    Until([&]{GuideCapture::SampleToken token;return GuideCapture::TryBeginSample(Stream::Fg,token)&&GuideCapture::TryRecord(token,r.list.Get(),in,stamp);},"full-resolution producer");
    f.Execute(r,f.producer.Get());f.Drain(f.producer.Get());Until([&]{return Observe(10).done;},"producer actual fence");
    NeverAcquire("no immutable publication before actual producer Reset");f.Reset(r);auto colour=Acquire(10);Fixture::Close(r);
    Check(colour.width==321&&colour.height==201&&colour.width>160&&colour.height>100,"actual selected full resolution, never offline proxy extent");
    Check(colour.consumer==Stream::Fg&&colour.source==stamp.source&&colour.epoch==stamp.epoch&&colour.feature==stamp.feature&&
        colour.configuration==stamp.configuration&&colour.owner==stamp.owner&&colour.tick==stamp.tick,"snapshot retains consumer/source/owner/configuration/timestamp");
    GuideCapture::Image offline;std::string offlineStatus;
    Check(!GuideCapture::Read(Stream::Fg,offline,offlineStatus)&&GuideCapture::DrainPairs(Stream::Fg).empty(),"normal GPU preview never publishes offline CPU images");
    auto pixels=f.Pixels(colour);for(unsigned y=0;y<201;++y)for(unsigned x=0;x<321;++x)
        PixelNear(pixels[size_t(y)*321+x],float(x+7)/512,float(y+5)/256,.25f,"actual production colour subrect pixels");
    GuideCapture::NoteDisplayed(colour);PreviewSnapshot displayed;
    Until([&]{return GuideCapture::ReadDisplayed(displayed);},"displayed metadata");
    Check(displayed.serial==colour.serial&&displayed.source==colour.source&&displayed.consumer==Stream::Fg&&displayed.tick==colour.tick&&
        !displayed.texture&&!displayed.lease,"displayed stamp identifies actual snapshot without storage ownership");
    auto cancelled=f.New();GuideCapture::SampleToken beforeFreeze;
    Until([&]{return GuideCapture::TryBeginSample(Stream::Fg,beforeFreeze);},"reservation before freezing");
    Demand(PreviewMode::FgColour,3,false,false,true);GuideCapture::SampleToken frozen;
    Check(!GuideCapture::NeedsOpenFgGuideRecording(),"freezing an actual colour snapshot does not retain raw FG guides");
    Check(!GuideCapture::TryRecord(beforeFreeze,cancelled.list.Get(),in,Stamp(999))&&beforeFreeze.slot==UINT_MAX,"freeze cancels an already reserved sample without recording");
    Fixture::Close(cancelled);
    Check(!GuideCapture::TryBeginSample(Stream::Fg,frozen),"frozen demand grants no new samples");
    auto still=Acquire(10);Check(still.serial==colour.serial&&still.tick==colour.tick,"freeze keeps immutable snapshot and true source age");
    Demand(PreviewMode::FgMotion,3,true);NeverAcquire("mode switch never returns previous colour snapshot");
    Check(GuideCapture::NeedsOpenFgGuideRecording(),"motion preview requires the original open guide recording");
    Check(!GuideCapture::ReadDisplayed(displayed),"mode switch clears previous displayed identity");
    auto motion=f.Complete(11,in);pixels=f.Pixels(motion);
    for(unsigned y=0;y<201;++y)for(unsigned x=0;x<321;++x){
        const unsigned sx=x+7,sy=5+200-y;const float mx=float(int(sx%17)-8)*.0025f*in.motionToUvX;
        const float my=float(int(sy%13)-6)*.00375f*in.motionToUvY;
        PixelNear(pixels[size_t(y)*321+x],.5f+mx*3,.5f+my*3,std::sqrt(mx*mx+my*my)*3,"asymmetric signed motion scale, subrect, flip and display gain");}
    Demand(PreviewMode::FgMotion,11,true);NeverAcquire("gain revision cannot display stale gain");
    auto gained=f.Complete(12,in);auto gainedPixels=f.Pixels(gained);bool changed=false;
    for(unsigned y=0;y<201;++y)for(unsigned x=0;x<321;++x){
        const float mx=float(int((x+7)%17)-8)*.0025f*in.motionToUvX,my=float(int((205-y)%13)-6)*.00375f*in.motionToUvY;
        const auto i=size_t(y)*321+x;PixelNear(gainedPixels[i],.5f+mx*11,.5f+my*11,std::sqrt(mx*mx+my*my)*11,"gain changes actual selected pixel shader output");
        changed|=gainedPixels[i]!=pixels[i];}
    Check(changed,"gain must change actual image pixels");
    // A retained immutable old lease cannot be drawn with a new mode revision.
    auto obsolete=f.New();Check(!GuideCapture::RetainRead(motion,obsolete.list.Get()),"old revision cannot gain new GPU readers");Fixture::Close(obsolete);
    colour={};still={};motion={};gained={};Demand(PreviewMode::FgDepth,3,false,true);
    auto depth=f.Complete(13,in);Check(depth.width==Fixture::Width&&depth.height==Fixture::Height,"raw selected actual texture retains full extent");
    pixels=f.Pixels(depth);for(unsigned y=0;y<Fixture::Height;++y)for(unsigned x=0;x<Fixture::Width;++x)
        PixelNear(pixels[size_t(y)*Fixture::Width+x],float(y)/256,float(y)/256,float(y)/256,"selected actual raw depth pixels");
    depth={};Demand(PreviewMode::Off);
}
static void TestHistoryInspectorPixels(Fixture& f){
    GuideCapture::PreviewDemand d;d.mode=PreviewMode::History;d.historyView=1;
    GuideCapture::SetPreviewDemand(d);
    Check(GuideCapture::HistoryDebugView()==1,"inspector subview shares the single existing history preview demand");
    auto in=f.Inputs();in.historyDiagnostic=true;in.historyView=1;
    auto s=f.Complete(15,in,Stream::History);
    Check(s.consumer==Stream::History&&s.width==321&&s.height==201,"history inspector retains actual full-resolution region");
    const auto pixels=f.Pixels(s);
    for(unsigned y=0;y<201;++y)for(unsigned x=0;x<321;++x)
        PixelNear(pixels[size_t(y)*321+x],float(x+7)/512,float(y+5)/256,.25f,
                  "already colourized diagnostic passes through without contribution heatmap or gain");
    d.historyView=2;GuideCapture::SetPreviewDemand(d);
    NeverAcquire("inspector view revision cannot show another diagnostic's pixels");
    auto oldReader=f.New();Check(!GuideCapture::RetainRead(s,oldReader.list.Get()),"changed inspector view cannot acquire old snapshot readers");
    Fixture::Close(oldReader);s={};
    GuideCapture::SampleToken token;
    Until([&]{return GuideCapture::TryBeginSample(Stream::History,token);},"reserve exact age subview");
    Check(token.historyView==2,"subview and revision captured together in the reservation");
    auto wrong=f.New();
    Check(!GuideCapture::TryRecord(token,wrong.list.Get(),in,Stamp(16)),
          "old rejection pixels cannot be relabelled as the newly reserved age view");
    Fixture::Close(wrong);
    in.historyView=2;auto age=f.Complete(17,in,Stream::History);
    Check(age.source==17,"matching exact inspector view can publish normally");
    age={};Demand(PreviewMode::Off);
}
static void TestDisplayedRetirement(Fixture& f){
    Demand(PreviewMode::FgColour);auto snapshot=f.Complete(14,f.Inputs());
    auto draw=f.MakeDraw(snapshot,false);GuideCapture::NoteDisplayed(snapshot);PreviewSnapshot displayed;
    // An empty or foreign identity must not clear the current display, even if
    // it happens to carry the same serial from a stale output owner.
    GuideCapture::ClearDisplayed({});
    Until([&]{return GuideCapture::ReadDisplayed(displayed)&&displayed.serial==snapshot.serial;},
        "empty retirement keeps current displayed identity");
    for(unsigned field=0;field<4;++field){auto foreign=snapshot;
        if(field==0)++foreign.serial;else if(field==1)++foreign.revision;
        else if(field==2)++foreign.epoch;else ++foreign.owner;
        GuideCapture::ClearDisplayed(foreign);
        Until([&]{return GuideCapture::ReadDisplayed(displayed)&&displayed.serial==snapshot.serial&&displayed.epoch==snapshot.epoch&&
            displayed.owner==snapshot.owner;},"mismatched retirement identity cannot clear current metadata");
    }
    {QueueGate gate(f,f.reader.Get());f.Execute(draw.commands,f.reader.Get());
        GuideCapture::ClearDisplayed(snapshot);
        Check(!GuideCapture::ReadDisplayed(displayed),"matching retirement clears displayed metadata");
        const auto pending=Observe(14);
        Check(pending.readers==1&&pending.readersDone==0&&pending.readersReset==0,
            "metadata clearing cannot complete or Reset a blocked actual reader");
        gate.Release();}
    f.Drain(f.reader.Get());Until([&]{return Observe(14).readersDone==1;},"reader fence after metadata clear");
    Check(Observe(14).readers==1&&Observe(14).readersReset==0,
        "cleared metadata cannot retire a completed reader before successful Reset");
    f.Reset(draw.commands);Fixture::Close(draw.commands);
    Until([&]{return Observe(14).readers==0;},"actual reader fence plus Reset after metadata clear");
    GuideCapture::NoteDisplayed(snapshot);
    // Publish a real newer snapshot in the same demand revision. Retirement of
    // the previous output must leave the replacement's displayed identity alone.
    auto producer=f.New();f.Record(producer,15,f.Inputs());f.Execute(producer,f.producer.Get());f.Drain(f.producer.Get());
    Until([&]{return Observe(15).done;},"replacement producer fence");
    f.Reset(producer);auto replacement=Acquire(15);Fixture::Close(producer);
    (void)f.Pixels(replacement);GuideCapture::NoteDisplayed(replacement);
    GuideCapture::ClearDisplayed(snapshot);
    Until([&]{return GuideCapture::ReadDisplayed(displayed)&&displayed.serial==replacement.serial&&displayed.epoch==replacement.epoch;},
        "stale output retirement preserves newer displayed snapshot");
    GuideCapture::ClearDisplayed(replacement);
    Check(!GuideCapture::ReadDisplayed(displayed),"replacement retirement clears its own displayed metadata");
    snapshot={};replacement={};Demand(PreviewMode::Off);
}
struct PreviewListDeviceAlias {
    inline static thread_local ID3D12Device* device=nullptr;
    inline static thread_local ID3D12GraphicsCommandList* selected=nullptr;
    using GetDevice=HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,REFIID,void**);
    inline static GetDevice original=nullptr;
    static HRESULT STDMETHODCALLTYPE Get(ID3D12GraphicsCommandList* list,REFIID iid,void** out){
        return list==selected?device->QueryInterface(iid,out):original(list,iid,out);}
};
static void TestDeviceAliasPixels(Fixture& f){
    ComPtr<ID3D12Device> alias;alias.Attach(new PreviewDeviceAlias(f.device.Get()));
    // D3D12 returns the same per-adapter device for repeated factories. Use a
    // different actual adapter for this negative control when hardware exists.
    ComPtr<ID3D12Device> foreign;
    ComPtr<IDXGIFactory4> factory;Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    for(UINT index=0;!foreign;++index){ComPtr<IDXGIAdapter1> adapter;
        if(factory->EnumAdapters1(index,&adapter)==DXGI_ERROR_NOT_FOUND)break;
        DXGI_ADAPTER_DESC1 desc{};Hr(adapter->GetDesc1(&desc));if(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)continue;
        if(FAILED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&foreign))))foreign.Reset();}
    if(!foreign)std::puts("GuidePreview: no hardware adapter; actual foreign-device negative control skipped");
    Check(alias.Get()!=f.device.Get()&&D3D12DeviceIdentity::Same(alias.Get(),f.device.Get()),
        "different ReShade contract interface resolves to the same actual WARP device");
    for(const auto mode:{PreviewMode::FgDepth,PreviewMode::FgMotion,PreviewMode::NrDepth,PreviewMode::NrMotion,PreviewMode::History}){
        Demand(mode);auto in=f.Inputs();in.contribution=f.inputs[0].Get();in.contributionRect=in.colourRect;
        auto r=f.New();GuideCapture::SampleToken token;const auto stream=GuideCapture::PreviewDetail::Selected(mode);
        const uint64_t source=700+static_cast<unsigned>(mode);
        // Exercise the same producer device-vs-resource mismatch as a proxy list
        // without replacing WARP execution or its real queues, fences and pixels.
        auto** table=*reinterpret_cast<void***>(r.list.Get());auto original=table[7];DWORD protection=0;
        Check(VirtualProtect(table+7,sizeof(void*),PAGE_EXECUTE_READWRITE,&protection)!=FALSE,"alias GetDevice slot protection");
        PreviewListDeviceAlias::device=alias.Get();PreviewListDeviceAlias::selected=r.list.Get();
        PreviewListDeviceAlias::original=reinterpret_cast<PreviewListDeviceAlias::GetDevice>(original);
        table[7]=reinterpret_cast<void*>(&PreviewListDeviceAlias::Get);
        try{
            Until([&]{return GuideCapture::TryBeginSample(stream,token)&&GuideCapture::TryRecord(token,r.list.Get(),in,Stamp(source));},
                "wrapper list and native input resources record a real preview");
        }catch(...){table[7]=original;DWORD ignored=0;VirtualProtect(table+7,sizeof(void*),protection,&ignored);throw;}
        table[7]=original;DWORD ignored=0;Check(VirtualProtect(table+7,sizeof(void*),protection,&ignored)!=FALSE,"restore real GetDevice method");
        f.Execute(r,f.producer.Get());f.Drain(f.producer.Get());Until([&]{return Observe(source).done;},"aliased producer actual GPU fence");
        f.Reset(r);auto snapshot=Acquire(source);Fixture::Close(r);auto pixels=f.Pixels(snapshot);
        Check(!pixels.empty(),"same-native preview produces output pixels");
        auto surface=std::static_pointer_cast<GuideCapture::PreviewDetail::Surface>(snapshot.lease);
        Check(surface->device.Get()==alias.Get()&&surface->display&&surface->display->device.Get()==f.device.Get(),
            "producer keeps proxy domain and actual output owns display objects");
        const auto first=pixels[0];
        if(mode==PreviewMode::FgDepth||mode==PreviewMode::NrDepth){const float z=std::log2(1+65535.f*(1-5.f/256))/16;
            PixelNear(first,z,z,z,"native output displays actual guide depth");}
        if(mode==PreviewMode::History)PixelNear(first,21.f/512,21.f/512,21.f/512,"native output displays actual contribution strength");
        if(mode==PreviewMode::FgMotion||mode==PreviewMode::NrMotion){const float x=.0025f*.75f,y=-.00375f*1.25f;
            PixelNear(first,.5f+x*3,.5f+y*3,std::sqrt(x*x+y*y)*3,"native output displays actual scaled guide motion");}
        if(foreign){ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> rejected;
            Hr(foreign->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
            Hr(foreign->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&rejected)));
            Check(!D3D12DeviceIdentity::Same(f.device.Get(),foreign.Get()),"negative control uses a distinct actual device");
            Check(!GuideCapture::RetainRead(snapshot,rejected.Get()),"a distinct actual device never gains a snapshot reader");Hr(rejected->Close());}
        snapshot={};Demand(PreviewMode::Off);
    }
}
static void TestProducerGate(Fixture& f){
    Demand(PreviewMode::FgColour);const auto before=Progress();auto r=f.New();f.Record(r,20,f.Inputs());
    {QueueGate gate(f,f.producer.Get());f.Execute(r,f.producer.Get());f.Reset(r);
        Check(Observe(20).reset,"successful Reset is attached to executing generation");
        f.Drain(f.reader.Get());NeverAcquire("producer Reset and unrelated queue fence cannot publish blocked work");
        const auto slot=ProgressSlot(20);
        Check(slot.source==20&&slot.epoch==1020&&slot.feature==101&&slot.configuration==202&&slot.owner==303&&
            slot.serial&&slot.recording&&slot.fence&&slot.fenceTarget&&slot.submitted&&slot.reset&&
            slot.list==reinterpret_cast<uintptr_t>(r.list.Get())&&slot.queue==reinterpret_cast<uintptr_t>(f.producer.Get())&&
            !slot.gpuCompleted&&!slot.published,"progress names the blocked actual producer list, queue, fence and exact source domain");
        Check(Observe(20).active&&!Observe(20).done&&Observe(20).inputsHeld,"blocked producer keeps actual inputs alive");gate.Release();}
    f.Drain(f.producer.Get());auto s=Acquire(20);Check(s.consumer==Stream::Fg,"real producer fence plus Reset publishes FG");
    const auto completed=ProgressSlot(20);const auto after=Progress();
    Check(completed.serial==s.serial&&completed.revision==s.revision&&completed.gpuCompleted&&completed.reset&&completed.published&&
        completed.list==reinterpret_cast<uintptr_t>(r.list.Get())&&completed.queue==reinterpret_cast<uintptr_t>(f.producer.Get())&&
        completed.fenceCompleted>=completed.fenceTarget,"progress only publishes the same immutable serial after its real producer fence and Reset");
    Check(after.recorded==before.recorded+1&&after.submitted==before.submitted+1&&after.gpuCompleted==before.gpuCompleted+1&&
        after.reset==before.reset+1&&after.published==before.published+1,"successful producer progress counters increment once per actual recording");
    Fixture::Close(r);s={};Demand(PreviewMode::Off);
}
static void TestDiscardedProducerProgress(Fixture& f){
    Demand(PreviewMode::FgColour);const auto before=Progress();auto recording=f.New();f.Record(recording,21,f.Inputs());
    Fixture::Close(recording);f.Reset(recording);
    Until([&]{return !Observe(21).active;},"unsubmitted producer retires only after actual successful Reset");
    const auto slot=ProgressSlot(21);const auto after=Progress();
    Check(slot.reset&&!slot.executing&&!slot.submitted&&!slot.signalled&&!slot.gpuCompleted&&!slot.published,
        "discarded producer Reset is not represented as submitted or GPU completed");
    Check(after.reset==before.reset+1&&after.submitted==before.submitted&&after.gpuCompleted==before.gpuCompleted&&
        after.published==before.published,"discarded producer only increments actual Reset progress");
    Fixture::Close(recording);Demand(PreviewMode::Off);
}
static void TestReaderRetirement(Fixture& f){
    Demand(PreviewMode::FgColour);auto s=f.Complete(30,f.Inputs());std::vector<Draw> readers;
    {QueueGate gate(f,f.reader.Get());
        for(unsigned i=0;i<8;++i){readers.push_back(f.MakeDraw(s,false));f.Execute(readers.back().commands,f.reader.Get());}
        auto extra=f.New();Check(!GuideCapture::RetainRead(s,extra.list.Get()),"eight fixed actual GPU reader recordings exhaust admission");Fixture::Close(extra);
        f.Reset(readers[0].commands);f.Drain(f.producer.Get());Sleep(20);
        auto o=Observe(30);Check(o.readers==8&&o.readersReset==1&&o.readersDone==0,"Reset and unrelated fence cannot release blocked actual readers");
        gate.Release();}
    f.Drain(f.reader.Get());Until([&]{const auto o=Observe(30);return o.readers==7&&o.readersDone==7;},"only fence+Reset reader retires");
    Check(Observe(30).readersReset==0,"completed executable readers remain pinned until each successful Reset");
    for(unsigned i=1;i<8;++i)f.Reset(readers[i].commands);
    Until([&]{return Observe(30).readers==0;},"all real reader fences and successful Resets retire");
    for(auto& draw:readers)Fixture::Close(draw.commands);s={};Demand(PreviewMode::Off);
}
static void TestDelayedReceipts(Fixture& f){
    Demand(PreviewMode::FgColour);auto r=f.New();f.Record(r,40,f.Inputs());ExecutionReceipt a,b;
    {QueueGate gate(f,f.producer.Get());f.Execute(r,f.producer.Get(),false);a=r.receipt;f.Reset(r);
        f.Record(r,41,f.Inputs());f.Execute(r,f.producer.Get(),false);b=r.receipt;
        Check(a.preview!=b.preview,"A Reset B captures distinct explicit recording generations");
        ID3D12CommandList* lists[]{r.list.Get()};GuideCapture::Submitted(f.producer.Get(),1,lists); // deliberate receipt-less negative.
        Sleep(20);Check(!Observe(40).post&&!Observe(41).post,"no-receipt post fails closed for both generations");
        Fixture::Post(r,f.producer.Get(),b);Fixture::Post(r,f.producer.Get(),b);f.Reset(r);
        Check(!Observe(40).post&&Observe(41).post,"B post before A acknowledges only captured B generation");
        NeverAcquire("B receipt cannot bypass blocked actual queue");gate.Release();}
    f.Drain(f.producer.Get());auto newest=Acquire(41);auto old=Observe(40);
    Check(old.active&&old.pre&&old.reset&&!old.post&&!old.published&&old.inputsHeld,"completed queue cannot guess delayed A receipt");
    Fixture::Post(r,f.producer.Get(),a);
    Until([&]{const auto o=Observe(40);return o.published&&!o.active;},"explicit delayed A receipt acknowledges A after B");
    auto still=Acquire(41);Check(still.serial==newest.serial,"late older A cannot regress latest B publication");
    Fixture::Close(r);newest={};still={};Demand(PreviewMode::Off);
}
static void TestBatchedHooks(Fixture& f){
    Demand(PreviewMode::FgColour);auto a=f.New(),b=f.New();f.Record(a,45,f.Inputs());f.Record(b,46,f.Inputs());
    Fixture::Close(a);Fixture::Close(b);
    // The real public wrapper canonicalizes in fixed chunks of sixteen. Both
    // sides of the chunk boundary must survive its returned merged receipt.
    std::vector<Recording> empty;std::array<ID3D12CommandList*,18> lists{};
    for(unsigned i=0;i<18;++i){empty.push_back(f.New());Fixture::Close(empty.back());lists[i]=empty.back().list.Get();}
    lists.front()=a.list.Get();lists.back()=b.list.Get();
    const auto receipt=HookFixture::FinishedPictureSubmitting(f.producer.Get(),UINT(lists.size()),lists.data());
    const auto nested=HookFixture::FinishedPictureSubmitting(f.producer.Get(),UINT(lists.size()),lists.data());
    Check(receipt.preview==nested.preview&&receipt.offline==nested.offline,"merged nested pre remains idempotent across chunk boundary");
    Check(std::count_if(receipt.preview.begin(),receipt.preview.end(),[](uint64_t n){return n!=0;})==2,"merged pre captures both actual producer generations beyond list sixteen");
    Check(std::all_of(receipt.offline.begin(),receipt.offline.end(),[](uint64_t n){return n==0;}),"GPU-only receipt has no offline generations");
    f.producer->ExecuteCommandLists(UINT(lists.size()),lists.data());
    HookFixture::FinishedPictureSubmitted(f.producer.Get(),UINT(lists.size()),lists.data(),receipt);
    f.Drain(f.producer.Get());NeverAcquire("batched real Execute still needs each producer Reset");
    f.Reset(a);f.Reset(b);auto latest=Acquire(46);
    Until([&]{const auto o=Observe(45);return o.published&&!o.active;},"first chunk producer independently acknowledges and retires");
    Fixture::Close(a);Fixture::Close(b);latest={};Demand(PreviewMode::Off);
}
// Run permanent-quarantine cases last: each consumes exactly one of the three
// fixed production slots. Never reset testing globals or enlarge the pool.
static void TestProducerReplay(Fixture& f){
    Demand(PreviewMode::FgColour);auto r=f.New();f.Record(r,80,f.Inputs());
    f.Execute(r,f.producer.Get());f.Drain(f.producer.Get());
    Until([&]{return Observe(80).done;},"first producer execution completed");
    // A repeated executable recording still reaches the real queue even when
    // capture quarantines it. Its first-use transitions must also be balanced.
    f.Execute(r,f.producer.Get(),true,false);f.Drain(f.producer.Get());
    Hr(f.device->GetDeviceRemovedReason());f.Reset(r);
    Check(Observe(80).quarantined&&Observe(80).active,"producer replay retains uncertain storage after actual completion and Reset");
    NeverAcquire("replayed producer cannot become immutable published input");
    Fixture::Close(r);Demand(PreviewMode::Off);
}
static void TestReaderReplay(Fixture& f){
    Demand(PreviewMode::FgColour);auto s=f.Complete(50,f.Inputs());auto d=f.MakeDraw(s,false);
    f.Execute(d.commands,f.reader.Get());f.Drain(f.reader.Get());
    Until([&]{return Observe(50).readersDone==1;},"first actual reader execution completion");
    // Draw-only target remains RENDER_TARGET; actual replay uses valid resource states.
    f.Execute(d.commands,f.reader.Get(),true,false);f.Drain(f.reader.Get());f.Reset(d.commands);
    Check(Observe(50).readers==1&&Observe(50).readersQuarantined==1,"actual replay remains quarantined after real fence and successful Reset");
    NeverAcquire("quarantined reader prevents publication/acquisition");
    Fixture::Close(d.commands);s={};Demand(PreviewMode::Off);Sleep(20);
    Check(Observe(50).readersQuarantined==1,"Off cannot free replay-uncertain reader storage");
}
static void TestQueueMismatch(Fixture& f){
    Demand(PreviewMode::FgColour);auto r=f.New();f.Record(r,60,f.Inputs());f.Execute(r,f.producer.Get(),false);
    Fixture::Post(r,f.reader.Get(),r.receipt);f.Drain(f.producer.Get());f.Drain(f.reader.Get());f.Reset(r);
    const auto o=Observe(60);Check(o.quarantined&&o.active&&o.inputsHeld&&!o.published,"receipt delivered on wrong actual queue fails closed");
    NeverAcquire("queue mismatch cannot publish after either actual fence or Reset");Fixture::Close(r);Demand(PreviewMode::Off);
}
static void TestSignalFailure(Fixture& f){
    Demand(PreviewMode::FgColour);auto r=f.New();f.Record(r,70,f.Inputs());
    {auto& state=GuideCapture::PreviewDetail::Get();std::lock_guard lock(state.mutex);state.failSignal=true;}
    f.Execute(r,f.producer.Get());f.Drain(f.producer.Get());f.Reset(r);
    Until([&]{return Observe(70).quarantined;},"injected existing test-only Signal failure");
    {auto& state=GuideCapture::PreviewDetail::Get();std::lock_guard lock(state.mutex);state.failSignal=false;}
    auto o=Observe(70);Check(o.active&&o.inputsHeld&&!o.signalled&&!o.published,"actual Execute plus failed producer Signal retains storage");
    NeverAcquire("external successful queue fence cannot replace failed owner Signal");Fixture::Close(r);Demand(PreviewMode::Off);Sleep(20);
    Check(Observe(70).quarantined&&Observe(70).inputsHeld,"Off cannot discard Signal-uncertain inputs");
    Demand(PreviewMode::FgColour);
    const auto end=GetTickCount64()+80;
    do{GuideCapture::SampleToken token;const bool admitted=GuideCapture::TryBeginSample(Stream::Fg,token);
        if(admitted)GuideCapture::CancelSample(token);
        Check(!admitted,"three uncertain production slots fail closed without pool growth");Sleep(2);
    }while(GetTickCount64()<end);
    Demand(PreviewMode::Off);
}
int main(int argc,char** argv){try{
    Fixture f;if(argc==2&&std::strcmp(argv[1],"--producer-replay")==0){
        TestProducerReplay(f);std::printf("GuidePreview producer replay WARP: %u checks passed; actual repeated Execute, no device removal\n",checks);return 0;
    }
    TestCallerPreviewAuthority(f);TestOpenFgGuideDemand();TestPixelsAndDemand(f);TestHistoryInspectorPixels(f);TestDisplayedRetirement(f);TestDeviceAliasPixels(f);TestProducerGate(f);TestDiscardedProducerProgress(f);TestReaderRetirement(f);TestDelayedReceipts(f);TestBatchedHooks(f);TestCompletedOwnerRing(f);
    TestReaderReplay(f);TestQueueMismatch(f);TestSignalFailure(f);
    std::printf("GuidePreview WARP: %u checks passed; production GPU compute/display, actual queues/fences/Reset; no NVIDIA SDK/game claims\n",checks);return 0;
}catch(const std::exception& e){GuideCapture::SetPreviewDemand({});std::fprintf(stderr,"GuidePreview WARP failure: %s\n",e.what());return 1;}}
