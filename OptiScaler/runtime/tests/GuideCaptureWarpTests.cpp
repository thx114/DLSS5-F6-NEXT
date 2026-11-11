// SPDX-License-Identifier: GPL-3.0-or-later
#include <runtime/GuideCapture.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <stdexcept>
#include <vector>

// Compile the exact public production hooks. The private NR watcher callback
// is deliberately a no-op: FG-only and Reset-unwatched lists still reach the
// sampler independently. No handwritten duplicate notification path is tested.
namespace HookFixture {
namespace Util {inline bool CheckForRealObject(const char*,IUnknown*,IUnknown**){return false;}}
namespace DetailReuse {inline ID3D12CommandList* RealSubmittedCommands(ID3D12CommandList* p){return p;}}
inline uint64_t finishedPictureSubmitCallbacks=0;
inline void FinishedPictureSubmittedReal(ID3D12CommandQueue*,UINT,ID3D12CommandList* const*){}
#include "GuideCaptureHooksProduction.inl"
}

using Microsoft::WRL::ComPtr;
using GuideCapture::Stream;
static unsigned checks=0;
static void Check(bool b,const char* what){++checks;if(!b)throw std::runtime_error(what);}
static void Hr(HRESULT result){Check(SUCCEEDED(result),"D3D call failed");}
static constexpr unsigned W=65,H=43;
struct Recording {
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
    // Keep submitted-generation allocators until real test queue drains.
    std::vector<ComPtr<ID3D12CommandAllocator>> oldAllocators;bool closed=false;
    GuideCapture::ExecutionReceipt receipt;
};
struct Fixture {
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;
    std::vector<ComPtr<ID3D12Resource>> uploads;
    Fixture(){
        ComPtr<IDXGIFactory4> factory;Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter> warp;Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
        Hr(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
        D3D12_COMMAND_QUEUE_DESC qd{};Hr(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)));
    }
    Recording New(){Recording r;Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&r.allocator)));
        Hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,r.allocator.Get(),nullptr,IID_PPV_ARGS(&r.list)));return r;}
    void Reset(Recording& r){
        Check(r.closed,"test Reset requires closed recording");
        ComPtr<ID3D12CommandAllocator> next;Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&next)));
        Hr(r.list->Reset(next.Get(),nullptr));r.oldAllocators.push_back(std::move(r.allocator));r.allocator=std::move(next);
        r.closed=false;GuideCapture::Reset(r.list.Get()); // only AFTER actual successful Reset
    }
    static void Close(Recording& r){Hr(r.list->Close());r.closed=true;}
    void Execute(Recording& r,bool post=true,bool pre=true){
        Close(r);ID3D12CommandList* lists[]{r.list.Get()};
        r.receipt={};
        if(pre){r.receipt=HookFixture::FinishedPictureSubmitting(queue.Get(),1,lists);
            const auto nested=HookFixture::FinishedPictureSubmitting(queue.Get(),1,lists);
            Check(r.receipt.preview==nested.preview&&r.receipt.offline==nested.offline,"nested pre captures the exact same generations");}
        queue->ExecuteCommandLists(1,lists);if(post)HookFixture::FinishedPictureSubmitted(queue.Get(),1,lists,r.receipt);
    }
    void Drain(){
        ComPtr<ID3D12Fence> fence;Hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));Hr(queue->Signal(fence.Get(),1));
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);Check(event!=nullptr,"create test event");
        Hr(fence->SetEventOnCompletion(1,event));const auto wait=WaitForSingleObject(event,10000);CloseHandle(event);
        Check(wait==WAIT_OBJECT_0,"real WARP queue completion");
    }
    ComPtr<ID3D12Resource> Texture(Recording& r,DXGI_FORMAT format,unsigned pixelBytes,const std::function<void(unsigned,unsigned,void*)>& fill){
        D3D12_RESOURCE_DESC td{};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;td.Width=W;td.Height=H;
        td.DepthOrArraySize=1;td.MipLevels=1;td.Format=format;td.SampleDesc.Count=1;D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> texture,upload;Hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&texture)));
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes=0;device->GetCopyableFootprints(&td,0,1,0,&fp,nullptr,nullptr,&bytes);
        D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=bytes;bd.Height=1;bd.DepthOrArraySize=1;
        bd.MipLevels=1;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;hp.Type=D3D12_HEAP_TYPE_UPLOAD;
        Hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload)));
        void* mapped=nullptr;Hr(upload->Map(0,nullptr,&mapped));
        for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x)fill(x,y,static_cast<unsigned char*>(mapped)+fp.Offset+size_t(y)*fp.Footprint.RowPitch+x*pixelBytes);
        upload->Unmap(0,nullptr);D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=upload.Get();
        from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=fp;to.pResource=texture.Get();
        to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;r.list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition={texture.Get(),0,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};r.list->ResourceBarrier(1,&b);
        uploads.push_back(upload);return texture;
    }
};
static GuideDiagnostics::Snapshot Stamp(uint64_t source,uint64_t domain=1){
    GuideDiagnostics::Snapshot s;s.source=source;s.epoch=source;s.feature=domain;s.configuration=domain;s.owner=domain;
    s.tick=GuideDiagnostics::Now();return s;
}
static void Record(Stream stream,Recording& r,const GuideCapture::Inputs& inputs,const GuideDiagnostics::Snapshot& stamp){
    const auto until=GetTickCount64()+10000;
    do{if(GuideCapture::Record(stream,r.list.Get(),inputs,stamp))return;Sleep(2);}while(GetTickCount64()<until);
    GuideCapture::Image im;std::string status;GuideCapture::Read(stream,im,status);throw std::runtime_error("Record timeout: "+status);
}
static GuideCapture::Image WaitImage(Stream stream,uint64_t source,uint64_t feature=1){
    GuideCapture::Image im;std::string status;const auto until=GetTickCount64()+10000;
    do{if(GuideCapture::Read(stream,im,status)&&im.stamp.source==source&&im.stamp.feature==feature)return im;Sleep(2);}while(GetTickCount64()<until);
    throw std::runtime_error("Read timeout: "+status);
}
static bool Current(Stream stream,uint64_t source,uint64_t feature=1){
    GuideCapture::Image im;std::string status;return GuideCapture::Read(stream,im,status)&&im.stamp.source==source&&im.stamp.feature==feature;
}
static std::vector<GuideCapture::Image> WaitPairs(size_t count){
    std::vector<GuideCapture::Image> result;const auto until=GetTickCount64()+10000;
    do{auto pairs=GuideCapture::DrainSourcePairs();for(auto& im:pairs)result.push_back(std::move(im));if(result.size()>=count)return result;Sleep(2);}while(GetTickCount64()<until);
    throw std::runtime_error("completed source-pair delivery timeout");
}
// Release even on failure, so a test cannot strand WARP at a queue wait.
struct QueueGate {
    ComPtr<ID3D12Fence> fence;bool released=false;
    explicit QueueGate(Fixture& f){Hr(f.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));Hr(f.queue->Wait(fence.Get(),1));}
    void Release(){Hr(fence->Signal(1));released=true;}
    ~QueueGate(){if(!released&&fence)fence->Signal(1);}
};
int main(){try{
    Fixture f;auto upload=f.New();std::array<ComPtr<ID3D12Resource>,3> textures;
    textures[0]=f.Texture(upload,DXGI_FORMAT_R32G32B32A32_FLOAT,16,[](unsigned x,unsigned y,void* p){float v[]{float(x)/64,float(y)/42,0.25f,1};std::memcpy(p,v,sizeof(v));});
    textures[1]=f.Texture(upload,DXGI_FORMAT_R32_FLOAT,4,[](unsigned,unsigned y,void* p){float v=float(y)/42;std::memcpy(p,&v,4);});
    textures[2]=f.Texture(upload,DXGI_FORMAT_R32G32_FLOAT,8,[](unsigned x,unsigned y,void* p){float v[]{float(x)*0.125f,-float(y)*0.25f};std::memcpy(p,v,sizeof(v));});
    auto fresh=f.Texture(upload,DXGI_FORMAT_R16G16B16A16_FLOAT,8,[](unsigned,unsigned,void* p){uint16_t v[]{0x3400,0x3800,0x3a00,0x3c00};std::memcpy(p,v,sizeof(v));});
    auto zero=f.Texture(upload,DXGI_FORMAT_R32G32B32A32_FLOAT,16,[](unsigned,unsigned,void* p){float v[]{.25f,.5f,.75f,1};std::memcpy(p,v,sizeof(v));});
    auto changed=f.Texture(upload,DXGI_FORMAT_R32G32B32A32_FLOAT,16,[](unsigned x,unsigned y,void* p){
        float v[]{x%2?.375f:.25f,.5f,.75f,1};if(!y)v[0]=std::numeric_limits<float>::quiet_NaN();std::memcpy(p,v,sizeof(v));});
    f.Execute(upload);f.Drain();
    GuideCapture::Inputs in;in.colour=textures[0].Get();in.depth=textures[1].Get();in.motion=textures[2].Get();
    in.colourRect=in.depthRect=in.motionRect={1,1,63,41};in.motionToUvX=1.f/63;in.motionToUvY=1.f/41;
    GuideCapture::Request(true,true);auto r=f.New();auto stamp=Stamp(2);
    Check(!GuideCapture::Record(Stream::Source,r.list.Get(),in,stamp),"must refuse borrowed game recording");in.independentRecording=true;
    Check(!GuideCapture::Record(Stream::Source,r.list.Get(),in,stamp),"initial preparation is asynchronous and skips recording");
    Record(Stream::Source,r,in,stamp);Check(!Current(Stream::Source,2),"recording is not completion");
    f.Execute(r,false);Check(!Current(Stream::Source,2),"Execute without post receipt is unknown");
    ID3D12CommandList* lists[]{r.list.Get()};HookFixture::FinishedPictureSubmitted(f.queue.Get(),1,lists,r.receipt);f.Drain();
    Sleep(20);Check(!Current(Stream::Source,2),"real producer fence without successful Reset cannot publish");
    f.Reset(r);auto image=WaitImage(Stream::Source,2);
    Check(image.width==63&&image.height==41,"odd subrect extent");Check(image.stamp.tick==stamp.tick,"source timestamp preserved");
    Check(image.colourPresent&&image.depthPresent&&image.motionPresent&&!image.contributionPresent,"real sample-presence flags");
    for(unsigned y=0;y<image.height;++y)for(unsigned x=0;x<image.width;++x){const auto i=y*image.width+x;
        Check(std::abs(image.colour[i].r-float(x+1)/64)<1e-6f,"colour x alignment");
        Check(std::abs(image.colour[i].g-float(y+1)/42)<1e-6f,"colour y alignment");
        Check(std::abs(image.depth[i]-float(y+1)/42)<1e-6f,"depth same-frame region");
        Check(image.motion[i].x==float(x+1)*.125f&&image.motion[i].y==-float(y+1)*.25f,"raw signed motion samples");
        Check(image.contribution[i]==-1,"unobserved contribution stays unknown");
    }
    auto pairs=WaitPairs(1);Check(pairs.size()==1&&pairs[0].stamp.source==2,"completed calibration source delivered once");
    Check(GuideCapture::DrainSourcePairs().empty(),"calibration does not replay source");

    // FG guide tagging does not authenticate final colour.
    auto guides=f.New();auto only=in;only.colour=nullptr;only.colourRect={};Record(Stream::Fg,guides,only,Stamp(3));
    f.Execute(guides);f.Drain();Check(!Current(Stream::Fg,3),"FG fence alone cannot publish executable recording");f.Reset(guides);
    image=WaitImage(Stream::Fg,3);Check(!image.colourPresent&&image.depthPresent&&image.motionPresent,"guide-only presence");
    Check(image.width==63&&image.height==41,"guide-only dimensions follow actual region");
    for(size_t i=0;i<image.colour.size();++i){Check(image.colour[i].r==0&&image.colour[i].a==0,"guide-only colour is absent");
        Check(image.contribution[i]==-1,"guide-only contribution unknown");}

    // Different formats; recording or confidence must not imply contribution.
    GuideCapture::Inputs history;history.colour=zero.Get();history.contribution=fresh.Get();history.compareFreshAnswer=true;history.independentRecording=true;
    auto hr=f.New();auto hs=Stamp(10,2);hs.fusionRecorded=true;hs.historyAvailable=true;hs.historySource=9;hs.tick-=3000;
    {QueueGate gate(f);Record(Stream::History,hr,history,hs);
        Check(!Current(Stream::History,10,2),"recorded fusion is not measured contribution");
        f.Execute(hr);Sleep(20);Check(!Current(Stream::History,10,2),"history measurement waits for its actual queue fence");gate.Release();}
    f.Drain();Check(!Current(Stream::History,10,2),"history fence alone cannot publish executable recording");f.Reset(hr);image=WaitImage(Stream::History,10,2);
    Check(image.stamp.contributionKnown&&image.stamp.contribution==0&&image.stamp.coverage==1,"identical actual RGB gives known zero contribution");
    Check(image.stamp.tick==hs.tick&&GuideDiagnostics::Stale(image.stamp),"late measured history retains stale age");
    Check(image.stamp.reason.find("not blend weight")!=std::string::npos,"metric distinguishes pixel delta from weight");
    for(float v:image.contribution)Check(v==0,"zero RGB-difference heatmap");
    history.colour=changed.Get();Record(Stream::History,hr,history,Stamp(11,2));f.Execute(hr);f.Drain();
    Check(!Current(Stream::History,11,2),"changed history awaits successful Reset");f.Reset(hr);image=WaitImage(Stream::History,11,2);
    Check(image.stamp.contributionKnown,"changed RGB contribution known only after GPU completion");
    Check(std::abs(image.stamp.coverage-42.f/43)<1e-6f,"nonfinite RGB excluded from measured coverage");
    Check(std::abs(image.stamp.contribution-32.f/65)<1e-6f,"changed sample ratio uses valid samples");
    for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x){const float v=image.contribution[y*W+x];
        Check(!y?v==-1:std::abs(v-(x%2?.125f:0))<1e-6f,"actual mixed-format RGB difference heatmap");}
    GuideDiagnostics::View view;Check(GuideDiagnostics::Read(view),"read passive completion");
    const auto& measured=view.lanes[static_cast<unsigned>(GuideDiagnostics::Consumer::NrHistory)].completed;
    Check(measured.source==11&&measured.contributionKnown&&measured.stage==GuideDiagnostics::Stage::Completed,"history measurement published after actual completion and Reset");

    // A real Reset can precede the delayed post hook. Neither CPU return nor
    // Reset permits publication while the actual queue is still blocked.
    auto delayed=f.New();auto ds=Stamp(20,3);
    {
        QueueGate gate(f);Record(Stream::Source,delayed,in,ds);f.Execute(delayed,false);
        const auto oldReceipt=delayed.receipt;
        f.Reset(delayed);Sleep(20);Check(!Current(Stream::Source,20,3),"Reset cannot complete an executing generation");
        Record(Stream::Source,delayed,in,Stamp(21,3));
        ID3D12CommandList* old[]{delayed.list.Get()};HookFixture::FinishedPictureSubmitted(f.queue.Get(),1,old,oldReceipt);
        Sleep(20);Check(!Current(Stream::Source,20,3),"post receipt cannot bypass actual queue delay");
        gate.Release();
    }
    f.Drain();image=WaitImage(Stream::Source,20,3);Check(image.stamp.tick==ds.tick,"late post retains source age");
    pairs=WaitPairs(1);Check(pairs.size()==1&&pairs[0].stamp.source==20,"Reset-before-post generation delivered exactly once");
    Check(!Current(Stream::Source,21,3),"old generation post cannot submit new recording");
    f.Execute(delayed,false);
    ID3D12CommandList* next[]{delayed.list.Get()};HookFixture::FinishedPictureSubmitted(f.queue.Get(),1,next,delayed.receipt);
    f.Drain();Check(!Current(Stream::Source,21,3),"new generation also requires its own Reset");f.Reset(delayed);
    image=WaitImage(Stream::Source,21,3);pairs=WaitPairs(1);
    Check(pairs.size()==1&&pairs[0].stamp.source==21,"next post completes only next generation");

    // Same list, A Reset B, with B acknowledged before delayed A. Offline
    // publication must preserve source order as well as exact generation identity.
    auto reverse=f.New();GuideCapture::ExecutionReceipt reverseA,reverseB;
    {
        QueueGate gate(f);Record(Stream::Source,reverse,in,Stamp(70,8));f.Execute(reverse,false);reverseA=reverse.receipt;
        f.Reset(reverse);Record(Stream::Source,reverse,in,Stamp(71,8));f.Execute(reverse,false);reverseB=reverse.receipt;
        Check(reverseA.offline!=reverseB.offline,"A and B prehooks capture distinct offline generations");
        ID3D12CommandList* same[]{reverse.list.Get()};
        HookFixture::FinishedPictureSubmitted(f.queue.Get(),1,same,reverseB);f.Reset(reverse);
        Sleep(20);Check(!Current(Stream::Source,70,8)&&!Current(Stream::Source,71,8),"B receipt and both Resets cannot bypass actual blocked queue");
        gate.Release();
    }
    f.Drain();Sleep(30);
    Check(!Current(Stream::Source,71,8)&&GuideCapture::DrainSourcePairs().empty(),"B post cannot guess A acknowledgement or bypass pending-source ordering");
    ID3D12CommandList* reversed[]{reverse.list.Get()};
    HookFixture::FinishedPictureSubmitted(f.queue.Get(),1,reversed,reverseA);
    HookFixture::FinishedPictureSubmitted(f.queue.Get(),1,reversed,reverseA);
    f.Drain();image=WaitImage(Stream::Source,71,8);pairs=WaitPairs(2);
    Check(pairs.size()==2&&pairs[0].stamp.source==70&&pairs[1].stamp.source==71,"late explicit A then already-acknowledged B publish each source exactly once");
    auto noReplacement=f.New();
    {
        QueueGate gate(f);Record(Stream::Fg,noReplacement,only,Stamp(4,7));f.Execute(noReplacement,false);
        f.Reset(noReplacement); // no new recording: both NR watch sets are absent
        ID3D12CommandList* old[]{noReplacement.list.Get()};
        HookFixture::FinishedPictureSubmitted(f.queue.Get(),1,old,noReplacement.receipt);
        Sleep(20);Check(!Current(Stream::Fg,4,7),"unwatched delayed FG post still waits for its actual queue");gate.Release();
    }
    f.Drain();image=WaitImage(Stream::Fg,4,7);
    Check(image.valid&&!image.colourPresent,"production post reaches FG-only Reset generation with no replacement recording");

    // Slots hold sources 4,2,3. Submit 4 and 3 first, delaying source 2's
    // receipt; physical slot order must never become temporal publication.
    auto a=f.New(),b=f.New(),c=f.New();
    Record(Stream::Source,a,in,Stamp(4,4));Record(Stream::Source,b,in,Stamp(2,4));Record(Stream::Source,c,in,Stamp(3,4));
    f.Execute(a);f.Execute(c);f.Execute(b,false);f.Drain();f.Reset(a);f.Reset(b);f.Reset(c);Sleep(30);
    Check(!Current(Stream::Source,4,4),"newer source waits for earlier pending source");
    Check(GuideCapture::DrainSourcePairs().empty(),"pending earlier source blocks out-of-order delivery");
    ID3D12CommandList* middle[]{b.list.Get()};HookFixture::FinishedPictureSubmitted(f.queue.Get(),1,middle,b.receipt);
    f.Drain();image=WaitImage(Stream::Source,4,4);pairs=WaitPairs(3);
    Check(pairs.size()==3&&pairs[0].stamp.source==2&&pairs[1].stamp.source==3&&pairs[2].stamp.source==4,"slots 4,2,3 publish sources 2,3,4");
    Record(Stream::Source,a,in,Stamp(1,4));f.Execute(a);f.Drain();f.Reset(a);Sleep(30);
    Check(Current(Stream::Source,4,4),"late older completion cannot regress latest image");
    Check(GuideCapture::DrainSourcePairs().empty(),"late older completion cannot regress source delivery");

    // Failed/abandoned Evaluate leaves an owned closed recording which is
    // actually Reset without ever executing. Repeated hot rebuilds must reclaim
    // discarded generations rather than permanently exhaust the three slots.
    auto abandoned=f.New();
    for(unsigned i=0;i<10;++i){Record(Stream::Source,abandoned,in,Stamp(50+i,5));Fixture::Close(abandoned);f.Reset(abandoned);}
    Check(!Current(Stream::Source,59,5),"discarded unsubmitted generation never publishes completion");
    Check(GuideCapture::DrainSourcePairs().empty(),"abandoned generations never enter calibration delivery");
    Record(Stream::Source,abandoned,in,Stamp(60,5));f.Execute(abandoned);f.Drain();f.Reset(abandoned);image=WaitImage(Stream::Source,60,5);
    pairs=WaitPairs(1);Check(pairs.size()==1&&pairs[0].stamp.source==60,"more-than-slot-count abandoned rebuilds still recover");

    // Missing pre with an actual post remains uncertain, even after Reset.
    auto missing=f.New();Record(Stream::NrModel,missing,in,Stamp(80,6));f.Execute(missing,true,false);
    f.Drain();Sleep(20);f.Reset(missing);Check(!Current(Stream::NrModel,80,6),"missing pre post remains quarantined after Reset");
    GuideCapture::Request(Stream::Fg,true);
    Check(GuideCapture::Wanted(Stream::Fg),"selected FG preview requests FG");
    Check(!GuideCapture::Wanted(Stream::Source)&&!GuideCapture::Wanted(Stream::NrModel)&&!GuideCapture::Wanted(Stream::History),"selected preview excludes other streams");
    Check(!GuideCapture::Record(Stream::Source,abandoned.list.Get(),in,Stamp(61,5)),"unselected stream records no GPU work");
    GuideCapture::Request(Stream::History,true,true);
    Check(GuideCapture::Wanted(Stream::History)&&GuideCapture::Wanted(Stream::Source),"explicit calibration adds only source to selected stream");
    Check(!GuideCapture::Wanted(Stream::Fg)&&!GuideCapture::Wanted(Stream::NrModel),"calibration excludes unrelated consumers");
    GuideCapture::Request(Stream::Count,true);
    Check(!GuideCapture::Wanted(),"invalid preview stream disables demand");
    GuideCapture::Request(Stream::Fg,true);
    Record(Stream::Fg,guides,only,Stamp(90,7));
    GuideCapture::Request(false);
    f.Execute(guides);f.Drain();f.Reset(guides);image=WaitImage(Stream::Fg,90,7);
    Check(image.valid,"demand expiry still completes actual queued recording");
    GuideCapture::Request(false);Check(!GuideCapture::Record(Stream::Source,abandoned.list.Get(),in,Stamp(61,5)),"disabled demand records no GPU work");
    // The detail panel must observe real RGB contribution without selecting a
    // full-screen preview, disturbing an explicit FG request, or copying images.
    GuideCapture::Request(Stream::Fg,true);
    GuideCapture::RequestHistoryMeasurement();
    Check(GuideCapture::OfflineWanted(Stream::Fg)&&GuideCapture::OfflineWanted(Stream::History),
          "history measurement leaves independent FG demand intact");
    Check(!GuideCapture::OfflineWanted(Stream::Source)&&!GuideCapture::OfflineWanted(Stream::NrModel),
          "history panel adds no source or model sampling");
    history.colour=changed.Get();auto measuredStamp=Stamp(95,12);
    measuredStamp.feature=measuredStamp.owner;
    Record(Stream::History,hr,history,measuredStamp);
    GuideCapture::ContributionSample metadata;
    GuideCapture::ReadHistoryMeasurement(metadata);
    Check(metadata.source!=95,"recorded history cannot claim measured contribution");
    f.Execute(hr);f.Drain();GuideCapture::ReadHistoryMeasurement(metadata);
    Check(metadata.source!=95,"real fence without Reset cannot claim measured contribution");
    f.Reset(hr);image=WaitImage(Stream::History,95,12);
    Check(GuideCapture::ReadHistoryMeasurement(metadata)&&metadata.known&&metadata.source==95,
          "metadata-only read returns actual completed history sample");
    Check(!GuideCapture::OfflineSampleDue(Stream::History),"completed measurement is rate limited before resource queries");
    Check(std::abs(metadata.changed-32.f/65)<1e-6f&&std::abs(metadata.coverage-42.f/43)<1e-6f,
          "panel uses measured RGB changes, not fusion readiness");
    const auto measuredNow=metadata.tick+100;
    Check(metadata.Matches(measuredStamp.owner,12,96,measuredNow),"matching current domain accepts recent sample");
    Check(!metadata.Matches(measuredStamp.owner+1,12,96,measuredNow),"different owner cannot reuse measurement");
    Check(!metadata.Matches(measuredStamp.owner,13,96,measuredNow),"new settings cannot reuse old measurement");
    Check(!metadata.Matches(measuredStamp.owner,12,94,measuredNow),"future source cannot appear current");
    Check(!metadata.Matches(measuredStamp.owner,12,96,metadata.tick+2001),"old sample cannot appear healthy");
    GuideCapture::RequestHistoryMeasurement(false);
    Check(!GuideCapture::OfflineWanted(Stream::History)&&GuideCapture::OfflineWanted(Stream::Fg),
          "closing detail measurement leaves unrelated demand intact");
    GuideCapture::Request(false);
    for(auto* recording:{&r,&guides,&hr,&delayed,&reverse,&noReplacement,&a,&b,&c,&abandoned,&missing})if(!recording->closed)Fixture::Close(*recording);
    std::printf("GuideCapture WARP: %u checks passed\n",checks);return 0;
}catch(const std::exception& e){GuideCapture::Request(false);std::fprintf(stderr,"GuideCapture WARP failure: %s\n",e.what());return 1;}}
