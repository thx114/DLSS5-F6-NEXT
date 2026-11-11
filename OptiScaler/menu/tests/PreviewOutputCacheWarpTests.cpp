// SPDX-License-Identifier: GPL-3.0-or-later
// Reuse the actual GuideCapture production/WARP fixture, without executing its
// main. Build this translation unit alone (no separate GuideCapture.cpp).
#define main GuidePreviewFixtureMain
#include "../../runtime/tests/GuidePreviewWarpTests.cpp"
#undef main
#include "../PreviewOutputCache.h"
#include <future>
#include <thread>

static Draw OutputTarget(Fixture& f)
{
    // Fixture::Inputs selects {7,5,321,201}; keep output at that exact extent.
    Draw d; d.commands=f.New(); d.width=321; d.height=201;
    D3D12_RESOURCE_DESC td{};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width=d.width;td.Height=d.height;td.DepthOrArraySize=1;td.MipLevels=1;
    td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.SampleDesc.Count=1;td.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
    Hr(f.device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&d.target)));
    D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=1;
    Hr(f.device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&d.rtv)));
    const auto rtv=d.rtv->GetCPUDescriptorHandleForHeapStart();
    f.device->CreateRenderTargetView(d.target.Get(),nullptr,rtv);
    d.commands.list->OMSetRenderTargets(1,&rtv,FALSE,nullptr);
    const float game[]{1,0,1,1};d.commands.list->ClearRenderTargetView(rtv,game,0,nullptr);
    return d;
}
static std::vector<std::array<unsigned char,4>> ReadOutput(Fixture& f,Draw& d)
{
    const auto td=d.target->GetDesc();UINT64 bytes=0;
    f.device->GetCopyableFootprints(&td,0,1,0,&d.footprint,nullptr,nullptr,&bytes);
    D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=bytes;bd.Height=1;
    bd.DepthOrArraySize=1;bd.MipLevels=1;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;
    Hr(f.device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&d.readback)));
    GuideCapture::Barrier(d.commands.list.Get(),d.target.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=d.target.Get();
    to.pResource=d.readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=d.footprint;
    d.commands.list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
    f.Execute(d.commands,f.reader.Get());f.Drain(f.reader.Get());f.Reset(d.commands);
    void* data=nullptr;const D3D12_RANGE range{0,size_t(bytes)};Hr(d.readback->Map(0,&range,&data));
    std::vector<std::array<unsigned char,4>> pixels(size_t(d.width)*d.height);
    for(unsigned y=0;y<d.height;++y)std::memcpy(pixels.data()+size_t(y)*d.width,
        static_cast<unsigned char*>(data)+d.footprint.Offset+size_t(y)*d.footprint.Footprint.RowPitch,size_t(d.width)*4);
    const D3D12_RANGE written{0,0};d.readback->Unmap(0,&written);Fixture::Close(d.commands);return pixels;
}
int main(){try{
    Fixture f;Demand(PreviewMode::FgColour);auto snapshot=f.Complete(901,f.Inputs());
    MenuPreview::OutputCache cache;auto demand=GuideCapture::GetPreviewDemand();cache.Select(demand,true);
    auto first=OutputTarget(f);bool read=false;
    Until([&]{return cache.Record(f.device.Get(),first.commands.list.Get(),first.target.Get(),&snapshot,read);},
        "first physical output records real production preview reader");
    Check(read,"first output owns a real snapshot reader");
    // Simulate an FPS glyph after preview: it must not get baked into the cache.
    const float white[]{1,1,1,1};const D3D12_RECT fps{0,0,8,8};
    first.commands.list->ClearRenderTargetView(first.rtv->GetCPUDescriptorHandleForHeapStart(),white,1,&fps);
    auto pixels=ReadOutput(f,first);cache.NoteSubmitted();
    PixelNear(pixels[0],1,1,1,"FPS layer remains above selected preview");
    // More physical outputs than the fixed runtime reader capacity. They must
    // not register additional snapshot readers or expose the magenta game image.
    for(unsigned frame=0;frame<24;++frame){
        auto next=OutputTarget(f);read=true;
        const auto* fresh=frame%2?&snapshot:nullptr; // transient acquire miss
        Check(cache.Record(f.device.Get(),next.commands.list.Get(),next.target.Get(),fresh,read)&&!read,
            "same snapshot/missed acquire uses menu pixels without a producer reader");
        auto shown=ReadOutput(f,next);cache.NoteSubmitted();
        PixelNear(shown[0],7.f/512,5.f/256,.25f,"cached image excludes FPS and normal game pixels");
        PixelNear(shown[100*next.width+200],207.f/512,105.f/256,.25f,"continuous output retains preview pixels");
    }
    // An existing immutable snapshot remains acquirable while a newer producer
    // waits for Reset. Complete() deliberately assumes no earlier snapshot.
    auto nextProducer=f.New();auto nextStamp=Stamp(902);nextStamp.epoch=snapshot.epoch;
    f.Record(nextProducer,f.Inputs(),nextStamp);
    f.Execute(nextProducer,f.producer.Get());f.Drain(f.producer.Get());
    Until([&]{return Observe(902).done;},"new producer completed before Reset");
    f.Reset(nextProducer);auto newer=Acquire(902);Fixture::Close(nextProducer);
    auto contention=OutputTarget(f);
    {
        std::promise<void> locked;
        std::jthread holder([&](std::stop_token stop){
            auto& state=GuideCapture::PreviewDetail::Get();std::lock_guard held(state.mutex);
            locked.set_value();while(!stop.stop_requested())std::this_thread::yield();
        });
        locked.get_future().get();
        Check(cache.Record(f.device.Get(),contention.commands.list.Get(),contention.target.Get(),&newer,read)&&!read,
            "real RetainRead try_lock failure retains same-domain display pixels");
    }
    auto contendedPixels=ReadOutput(f,contention);
    PixelNear(contendedPixels[0],7.f/512,5.f/256,.25f,"reader rejection never exposes normal game image");
    demand.frozen=true;cache.Select(demand,true);Check(cache.HasImage(),"freeze retains selected view");
    demand.gain+=1;cache.Select(demand,true);Check(!cache.HasImage(),"gain revision invalidates pixels");
    auto changed=OutputTarget(f);read=true;
    Check(!cache.Record(f.device.Get(),changed.commands.list.Get(),changed.target.Get(),nullptr,read)&&!read,
        "new view cannot borrow old pixels");
    (void)ReadOutput(f,changed);
    demand=GuideCapture::GetPreviewDemand();cache.Select(demand,true);
    auto restored=OutputTarget(f);
    Until([&]{return cache.Record(f.device.Get(),restored.commands.list.Get(),restored.target.Get(),&snapshot,read);},
        "same storage can refresh after actual view invalidation");
    (void)ReadOutput(f,restored);cache.NoteSubmitted();
    // Only the source epoch changes. A failed reader admission must not fall
    // back to cached pixels from the previous epoch, even with all other domain
    // fields and the selected demand revision unchanged.
    auto epochProducer=f.New();auto epochStamp=Stamp(903);epochStamp.epoch=snapshot.epoch+1;
    f.Record(epochProducer,f.Inputs(),epochStamp);f.Execute(epochProducer,f.producer.Get());f.Drain(f.producer.Get());
    Until([&]{return Observe(903).done;},"new epoch producer fence");
    f.Reset(epochProducer);auto nextEpoch=Acquire(903);Fixture::Close(epochProducer);
    Check(nextEpoch.revision==snapshot.revision&&nextEpoch.owner==snapshot.owner&&nextEpoch.feature==snapshot.feature&&
        nextEpoch.configuration==snapshot.configuration&&nextEpoch.epoch!=snapshot.epoch,"actual snapshot changes only the source epoch domain");
    auto epochContention=OutputTarget(f);
    {
        std::promise<void> locked;
        std::jthread holder([&](std::stop_token stop){
            auto& state=GuideCapture::PreviewDetail::Get();std::lock_guard held(state.mutex);
            locked.set_value();while(!stop.stop_requested())std::this_thread::yield();
        });
        locked.get_future().get();read=true;
        Check(!cache.Record(f.device.Get(),epochContention.commands.list.Get(),epochContention.target.Get(),&nextEpoch,read)&&
            !read&&!cache.HasImage(),"epoch change invalidates old pixels before real reader try_lock failure");
    }
    auto epochPixels=ReadOutput(f,epochContention);
    PixelNear(epochPixels[0],1,0,1,"failed new epoch refresh never copies the old epoch cache");
    auto restoredEpoch=OutputTarget(f);
    Until([&]{return cache.Record(f.device.Get(),restoredEpoch.commands.list.Get(),restoredEpoch.target.Get(),&snapshot,read);},
        "refresh invalidated cache from an available immutable snapshot");
    (void)ReadOutput(f,restoredEpoch);cache.NoteSubmitted();
    auto foreign=snapshot;foreign.owner++;Check(cache.NeedsRefresh(foreign)&&!cache.HasImage(),
        "known owner change rejects cached previous domain");
    cache.Select(demand,false);auto source=OutputTarget(f);
    Check(!cache.Record(f.device.Get(),source.commands.list.Get(),source.target.Get(),&snapshot,read),
        "FG source route cannot record display preview");
    (void)ReadOutput(f,source);
    cache.Select({},true);Check(!cache.HasImage(),"Off removes display demand");
    cache.Retire();PreviewSnapshot displayed;
    Check(!GuideCapture::ReadDisplayed(displayed),"cache retirement clears last submitted metadata even after eligibility was invalidated");
    MenuPreview::OutputCache oldOutput,newOutput;
    const auto submitOutput=[&](MenuPreview::OutputCache& output,const PreviewSnapshot& image){
        output.Select(demand,true);auto target=OutputTarget(f);
        Until([&]{return output.Record(f.device.Get(),target.commands.list.Get(),target.target.Get(),&image,read);},
            "replacement output records its actual immutable snapshot");
        (void)ReadOutput(f,target);output.NoteSubmitted();
    };
    submitOutput(oldOutput,snapshot);submitOutput(newOutput,newer);
    oldOutput.Retire();
    Until([&]{return GuideCapture::ReadDisplayed(displayed)&&displayed.serial==newer.serial;},
        "retiring old output cannot clear newer output metadata in the same demand revision");
    // Record a new image, then discard it without Execute. Retirement still
    // identifies the last submitted snapshot rather than the unsubmitted one.
    auto discarded=OutputTarget(f);
    Until([&]{return newOutput.Record(f.device.Get(),discarded.commands.list.Get(),discarded.target.Get(),&nextEpoch,read);},
        "record optional refresh before discarding its recording");
    Fixture::Close(discarded.commands);f.Reset(discarded.commands);
    Fixture::Close(discarded.commands);newOutput.DiscardRecording();newOutput.Retire();
    Check(!GuideCapture::ReadDisplayed(displayed),"retirement after discarded refresh clears the last submitted display identity");
    Demand(PreviewMode::Off);
    std::printf("Menu preview output WARP: %u checks passed; real output pixels, continuous copy and FPS order; no game claims\n",checks);return 0;
}catch(const std::exception& e){Demand(PreviewMode::Off);std::fprintf(stderr,"Menu output WARP failure: %s\n",e.what());return 1;}}
