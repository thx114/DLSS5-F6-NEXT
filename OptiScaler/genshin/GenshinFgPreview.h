// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Windows.h>
#include <SysUtils.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>

// Observation of the exact accepted FG tags. No shader, guide replacement,
// CPU/GPU wait, game-context operation or dependency on NR/menu renderer API.
namespace GenshinFgPreview {
using Microsoft::WRL::ComPtr;
inline constexpr unsigned W=96,H=60,N=W*H;
// Two bounded rectangle copies, never a full-resolution staging texture. Each
// snapshot is ONE tile from ONE accepted tag pair, not a temporal composite.
inline constexpr unsigned TileWidth=192,TileHeight=120,TileCount=9;
inline constexpr UINT64 MaxBytes=128ull*1024*1024;
inline constexpr uint64_t PendingTimeoutMs=5000;
inline constexpr uint64_t SnapshotStaleMs=1500;
struct Region {
    unsigned x=0,y=0,width=0,height=0,index=0,count=0;
};
inline Region SampleRegion(unsigned width,unsigned height,unsigned index) noexcept {
    Region r;r.width=(std::min)(width,TileWidth);r.height=(std::min)(height,TileHeight);
    r.count=(width==r.width && height==r.height)?1:TileCount;r.index=index%r.count;
    r.x=(width-r.width)*(r.index%3)/2;r.y=(height-r.height)*(r.index/3)/2;
    return r;
}
struct Stamp {
    uint64_t generation=0,sourceFrame=0,picture=0,providerFrame=0,tick=0;
    unsigned pictureKind=0;
    bool submitted=false;
};
struct Snapshot {
    Stamp stamp;
    // width/height remain the accepted source extent. Arrays are the 96x60
    // center samples of region ONLY, in original source coordinates. No other
    // region is implied valid; UI must label the crop instead of a full frame.
    Region region;
    UINT64 copiedBytes=0,readbackBytes=0;
    uint64_t completedTick=0,ageMs=0;
    bool fullFrame=false,composite=false,stale=true;
    unsigned width=0,height=0,validDepth=0,validMotion=0,zeroMotion=0;
    float minDepth=0,maxDepth=0,maxMotion=0;
    std::array<float,N> depth{};
    std::array<std::array<float,2>,N> motion{};
};
struct Pending {
    Stamp stamp;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Resource> readback,depth,motion;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12CommandQueue> queue;
    std::shared_ptr<void> owner;
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT,2> layout{};
    UINT64 bytes=0;
    unsigned width=0,height=0;
    Region region;
    UINT64 fenceValue=0;
    bool signalled=false,retired=false;
};
// Independent, nonblocking trace lanes: at most one report/lane/second,
// plus a 10-second heartbeat. A changing refusal cannot flood the log.
enum class TraceStage : unsigned { Control, Demand, Capture, Copy, Completion, Draw, Refusal, Count };
struct TraceLane {
    std::atomic_flag busy=ATOMIC_FLAG_INIT;
    const char* reason=nullptr;
    unsigned state=0;
    uint64_t tick=0,suppressed=0;
};
using VisibilityCheck=bool(*)() noexcept;
struct Store {
    std::mutex mutex;
    std::atomic<uint64_t> wantedUntil{0};
    std::atomic<VisibilityCheck> menuVisible{nullptr};
    std::array<TraceLane,static_cast<unsigned>(TraceStage::Count)> trace;
    uint64_t lastCapture=0;
    unsigned nextTile=4; // Center first, then rotate the nine spatial anchors.
    // Exactly one slot: pending (including quarantined) OR completed idle.
    std::unique_ptr<Pending> pending,idle;
    Snapshot ready;
    const char* status="Waiting for accepted FG depth and motion tags.";
};
// Bounded process lifetime: uncertain GPU work must never be torn down/reused.
inline Store& Storage(){static auto* s=new Store;return *s;}
inline void Trace(TraceStage stage,const char* reason,unsigned state=0,Stamp stamp={}) noexcept {
    auto& lane=Storage().trace[static_cast<unsigned>(stage)];
    if(lane.busy.test_and_set(std::memory_order_acquire))return;
    const auto now=GetTickCount64();
    const bool changed=!lane.reason || std::strcmp(lane.reason,reason)!=0 || lane.state!=state;
    if(lane.reason && (now-lane.tick<1000 || (!changed && now-lane.tick<10000)))++lane.suppressed;
    else {
        static constexpr const char* names[]={"control","demand","capture","copy","completion","draw","refusal"};
        try {
            LOG_INFO("R845 FG preview state: pid={} stage={} reason={} state={} generation={} provider_frame={} source_frame={} picture={} submitted={} suppressed={} accepted_tags_only=true visual=UNKNOWN",
                GetCurrentProcessId(),names[static_cast<unsigned>(stage)],reason,state,stamp.generation,
                stamp.providerFrame,stamp.sourceFrame,stamp.picture,stamp.submitted,lane.suppressed);
        }catch(...){}
        lane.reason=reason;lane.state=state;lane.tick=now;lane.suppressed=0;
    }
    lane.busy.clear(std::memory_order_release);
}
inline void Request(bool wanted,const char* reason="menu",VisibilityCheck visible=nullptr) noexcept {
    auto& s=Storage();
    s.menuVisible.store(visible,std::memory_order_release);
    s.wantedUntil.store(wanted?GetTickCount64()+750:0,std::memory_order_release);
    Trace(TraceStage::Demand,reason,wanted?1:0);
}
inline float Half(uint16_t h) noexcept {
    const unsigned sign=unsigned(h&0x8000)<<16,exponent=(h>>10)&31,mantissa=h&1023;
    if(!exponent) return (h&0x8000?-1.0f:1.0f)*std::ldexp(float(mantissa),-24);
    return std::bit_cast<float>(sign | (exponent==31?0x7f800000u:(exponent+112)<<23) | (mantissa<<13));
}
inline bool ValidMotion(float x,float y) noexcept {
    return std::isfinite(x) && std::isfinite(y) && std::abs(x)<65000 && std::abs(y)<65000;
}
// Caller holds mutex. Retired/error/stale slots retain every GPU input forever.
// This blocks further captures rather than growing a quarantine or reusing work.
inline void Retire(Store& s,Pending& p,const char* status) noexcept {
    p.retired=true;s.status=status;
    Trace(TraceStage::Refusal,status,1,p.stamp);
}
// Caller holds mutex. Only actual completion of our copy permits Map/reuse.
inline void Poll(Store& s) noexcept {
    if(!s.pending)return;
    auto& p=*s.pending;
    if(p.retired)return;
    if(!p.signalled){Retire(s,p,"Preview copy signal failed; retained, restart required.");return;}
    if(GetTickCount64()-p.stamp.tick>PendingTimeoutMs) {
        Retire(s,p,"Preview copy stale; retained, restart required.");return;
    }
    const auto done=p.fence->GetCompletedValue();
    if(done==UINT64_MAX){Retire(s,p,"Preview device removed; inputs retained.");return;}
    if(done<p.fenceValue){s.status="Preview GPU copy pending (no wait).";return;}
    void* mapped=nullptr;const D3D12_RANGE read{0,SIZE_T(p.bytes)};
    if(FAILED(p.readback->Map(0,&read,&mapped)) || !mapped) {
        Retire(s,p,"Completed preview readback could not be mapped; retained, restart required.");return;
    }
    Snapshot image;image.stamp=p.stamp;image.width=p.width;image.height=p.height;
    image.region=p.region;image.completedTick=GetTickCount64();
    image.copiedBytes=UINT64(p.region.width)*p.region.height*12;image.readbackBytes=p.bytes;
    image.fullFrame=p.region.width==p.width && p.region.height==p.height;
    image.minDepth=INFINITY;image.maxDepth=-INFINITY;
    const auto* base=static_cast<const unsigned char*>(mapped);
    for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x) {
        const unsigned sx=(std::min)(p.region.width-1,unsigned((uint64_t(2*x+1)*p.region.width)/(2*W)));
        const unsigned sy=(std::min)(p.region.height-1,unsigned((uint64_t(2*y+1)*p.region.height)/(2*H)));
        const auto i=y*W+x;
        float z;uint16_t mv[2];
        std::memcpy(&z,base+p.layout[0].Offset+SIZE_T(sy)*p.layout[0].Footprint.RowPitch+sx*4,4);
        // NormalizeNative stores RG velocity in an RGBA16 pixel; ignore BA.
        std::memcpy(mv,base+p.layout[1].Offset+SIZE_T(sy)*p.layout[1].Footprint.RowPitch+sx*8,4);
        image.depth[i]=z;image.motion[i]={Half(mv[0]),Half(mv[1])};
        if(std::isfinite(z) && z>=0 && z<=1) {
            ++image.validDepth;image.minDepth=(std::min)(image.minDepth,z);image.maxDepth=(std::max)(image.maxDepth,z);
        }
        const auto vx=image.motion[i][0],vy=image.motion[i][1];
        if(ValidMotion(vx,vy)) {
            ++image.validMotion;
            image.maxMotion=(std::max)(image.maxMotion,std::sqrt(vx*vx+vy*vy));
            if(std::abs(vx)<0.01f && std::abs(vy)<0.01f)++image.zeroMotion;
        }
    }
    const D3D12_RANGE written{0,0};p.readback->Unmap(0,&written);
    if(!image.validDepth)image.minDepth=image.maxDepth=0;
    s.ready=std::move(image);
    p.depth.Reset();p.motion.Reset();p.owner.reset();
    // The same allocator/list/fence/readback can now be used at the next sample.
    // The queue/device remain pinned; a different queue or layout replaces idle.
    s.idle=std::move(s.pending);s.status="Completed accepted-tag tile (partial source, not a composite).";
    Trace(TraceStage::Completion,"fence-completed-map-succeeded",0,s.ready.stamp);
}
inline bool Read(Snapshot& image,const char*& status) noexcept {
    auto& s=Storage();std::unique_lock lock(s.mutex,std::try_to_lock);
    if(!lock.owns_lock()){status="Preview busy; waiting for the next draw.";return false;}
    Poll(s);image=s.ready;status=s.status;
    image.ageMs=image.stamp.tick?GetTickCount64()-image.stamp.tick:0;
    image.stale=!image.stamp.tick || image.ageMs>SnapshotStaleMs;
    return image.stamp.tick!=0;
}
inline void DispatchResult(uint64_t generation,uint64_t providerFrame,bool submitted) noexcept {
    auto& s=Storage();std::lock_guard lock(s.mutex);
    const auto update=[&](Stamp& t){if(t.generation==generation && t.providerFrame==providerFrame)t.submitted=submitted;};
    if(s.pending)update(s.pending->stamp);update(s.ready.stamp);
}
// Called AFTER all three real SetGenshinResource calls accepted, on that exact
// provider queue AFTER its producer/copy waits and BEFORE provider Present.
// Both source states are COMMON at this boundary; restore them on the same list.
inline bool Capture(ID3D12Device* device,ID3D12CommandQueue* queue,ID3D12Resource* depth,
                    ID3D12Resource* motion,std::shared_ptr<void> owner,Stamp stamp) noexcept {
    try {
        auto& s=Storage();const auto now=GetTickCount64();
        Trace(TraceStage::Capture,"accepted-tags-reached",0,stamp);
        const auto wanted=s.wantedUntil.load(std::memory_order_acquire);
        const auto visible=s.menuVisible.load(std::memory_order_acquire);
        if(!wanted || now>wanted || (visible && !visible())) {
            Trace(TraceStage::Refusal,!wanted?"demand-disabled":now>wanted?"demand-expired":"menu-hidden",0,stamp);
            return false;
        }
        std::unique_lock lock(s.mutex,std::try_to_lock);
        if(!lock.owns_lock()){Trace(TraceStage::Refusal,"storage-busy",0,stamp);return false;}
        Poll(s);
        if(s.pending){Trace(TraceStage::Refusal,s.status,s.pending->retired?1:0,stamp);return false;}
        if(s.lastCapture && now-s.lastCapture<500){Trace(TraceStage::Refusal,"sample-2Hz-limit",0,stamp);return false;}
        const auto refuse=[&](const char* reason) {
            s.status=reason;Trace(TraceStage::Refusal,reason,0,stamp);return false;
        };
        if(!device || !queue || !depth || !motion || !owner || !stamp.generation || !stamp.providerFrame ||
           queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT)return refuse("Preview requires valid accepted tags, owner and a DIRECT queue.");
        for(ID3D12DeviceChild* child:{static_cast<ID3D12DeviceChild*>(queue),static_cast<ID3D12DeviceChild*>(depth),static_cast<ID3D12DeviceChild*>(motion)}) {
            ComPtr<ID3D12Device> actual;
            if(FAILED(child->GetDevice(IID_PPV_ARGS(&actual))) || actual.Get()!=device)return refuse("Preview queue or accepted resource belongs to a different device.");
        }
        const auto dd=depth->GetDesc(),md=motion->GetDesc();
        if(dd.Format!=DXGI_FORMAT_R32_FLOAT || md.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT ||
           dd.Width!=md.Width || dd.Height!=md.Height || !dd.Width || !dd.Height || dd.Width>16384 || dd.Height>16384)return refuse("Preview accepted-tag formats or matching extents refused.");
        for(const auto& d:{dd,md})if(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.DepthOrArraySize!=1 ||
            d.MipLevels!=1 || d.SampleDesc.Count!=1 || d.SampleDesc.Quality || d.Layout!=D3D12_TEXTURE_LAYOUT_UNKNOWN)return refuse("Preview accepted-tag dimension, mip, sample or layout refused.");
        const auto region=SampleRegion(unsigned(dd.Width),dd.Height,s.nextTile);
        auto tileDepth=dd,tileMotion=md;
        tileDepth.Width=tileMotion.Width=region.width;tileDepth.Height=tileMotion.Height=region.height;
        std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT,2> layout{};
        UINT64 depthBytes=0,motionBytes=0;
        device->GetCopyableFootprints(&tileDepth,0,1,0,&layout[0],nullptr,nullptr,&depthBytes);
        const auto offset=(depthBytes+511)&~UINT64(511);
        device->GetCopyableFootprints(&tileMotion,0,1,offset,&layout[1],nullptr,nullptr,&motionBytes);
        // pTotalBytes is the footprint size, independent of BaseOffset.
        const auto bytes=layout[1].Offset+UINT64(layout[1].Footprint.RowPitch)*region.height;
        if(!depthBytes || !motionBytes || !bytes || bytes>MaxBytes)return refuse("Preview readback exceeds the 128MiB limit or has an invalid footprint.");
        const bool reuse=s.idle && s.idle->device.Get()==device && s.idle->queue.Get()==queue &&
            s.idle->width==dd.Width && s.idle->height==dd.Height && s.idle->bytes==bytes;
        // Drop only completed idle storage BEFORE replacement allocation so the
        // readback budget cannot temporarily double during a resolution change.
        if(!reuse)s.idle.reset();
        if(!s.idle)s.idle=std::make_unique<Pending>();
        auto& p=s.idle;
        const auto fail=[&](const char* message) {
            Retire(s,*p,message);s.pending=std::move(p);return false;
        };
        if(reuse) {
            if(p->fenceValue>=UINT64_MAX-1)return fail("Preview fence exhausted; retained, restart required.");
            if(FAILED(p->allocator->Reset()) || FAILED(p->list->Reset(p->allocator.Get(),nullptr)))
                return fail("Preview command reset failed; retained, restart required.");
        } else {
            p->device=device;p->queue=queue;p->width=unsigned(dd.Width);p->height=dd.Height;
            p->layout=layout;p->bytes=bytes;
            D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=p->bytes;
            buffer.Height=1;buffer.DepthOrArraySize=buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&p->readback))) ||
               FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&p->allocator))) ||
               FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,p->allocator.Get(),nullptr,IID_PPV_ARGS(&p->list))) ||
               FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&p->fence))))
                return fail("Preview resource creation failed; retained, restart required.");
        }
        ++p->fenceValue;p->signalled=false;p->region=region;
        D3D12_RESOURCE_BARRIER barriers[2]{};
        ID3D12Resource* sources[]={depth,motion};
        for(unsigned i=0;i<2;++i) {
            barriers[i].Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;auto& b=barriers[i].Transition;
            b.pResource=sources[i];b.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.StateBefore=D3D12_RESOURCE_STATE_COMMON;b.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
        }
        p->list->ResourceBarrier(2,barriers);
        const D3D12_BOX box{region.x,region.y,0,region.x+region.width,region.y+region.height,1};
        for(unsigned i=0;i<2;++i) {
            D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=sources[i];from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            to.pResource=p->readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=p->layout[i];
            p->list->CopyTextureRegion(&to,0,0,0,&from,&box);
            std::swap(barriers[i].Transition.StateBefore,barriers[i].Transition.StateAfter);
        }
        p->list->ResourceBarrier(2,barriers);
        if(FAILED(p->list->Close()))return fail("Preview command close failed; retained, restart required.");
        // A switch/page/menu close during preparation must not submit a copy.
        const auto submitNow=GetTickCount64();
        const auto submitWanted=s.wantedUntil.load(std::memory_order_acquire);
        const auto submitVisible=s.menuVisible.load(std::memory_order_acquire);
        if(!submitWanted || submitNow>submitWanted || (submitVisible && !submitVisible()))
            return refuse("Preview demand withdrawn before GPU submission.");
        stamp.tick=submitNow;p->stamp=stamp;p->depth=depth;p->motion=motion;p->owner=std::move(owner);p->queue=queue;
        // Retain before the first GPU command. A failed signal keeps the entire
        // single job forever; subsequent captures cannot recycle its owner.
        s.pending=std::move(p);s.lastCapture=submitNow;auto& job=*s.pending;
        ID3D12CommandList* lists[]={job.list.Get()};queue->ExecuteCommandLists(1,lists);
        s.nextTile=(region.index+1)%region.count;
        job.signalled=SUCCEEDED(queue->Signal(job.fence.Get(),job.fenceValue));
        if(job.signalled)s.status="Preview GPU copy pending (no wait).";
        else Retire(s,job,"Preview copy signal failed; retained, restart required.");
        Trace(TraceStage::Copy,job.signalled?"copy-submitted-fence-signalled":"copy-signal-failed",reuse?1:0,stamp);
        return job.signalled;
    }catch(...){Trace(TraceStage::Refusal,"capture-exception",0,stamp);return false;}
}
}
