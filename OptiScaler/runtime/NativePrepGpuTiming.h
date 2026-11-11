// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>

namespace NativePrepGpuTiming {
struct Identity {
    uint64_t frame=0,sourceEpoch=0,sourceGeneration=0,sourceFrame=0,pictureSerial=0;
    uintptr_t picture=0;
};
struct Reading {
    bool known=false;
    Identity input{};
    uintptr_t queue=0,fence=0;
    uint64_t target=0,completed=0,frequency=0;
    std::array<double,3> milliseconds{}; // flow; trust/cut; guide construction
};
// Observations only. No waits, submissions, scheduling or resource authority.
// All four stamps precede the consumer on the same producer command list.
// A split consumer/final filter may extend the final fence, never shorten it.
template<unsigned Slots> class Recorder final {
    template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
    struct Slot {
        Identity input{};
        Ptr<ID3D12Fence> fence;
        uint64_t value=0;
        unsigned stamps=0;
        bool resolved=false;
    };
    Ptr<ID3D12QueryHeap> heap_;
    Ptr<ID3D12Resource> readback_;
    Ptr<ID3D12CommandQueue> queue_;
    uint64_t frequency_=0;
    bool attempted_=false;
    std::array<Slot,Slots> slots_{};
    bool Disable() noexcept {
        heap_.Reset();readback_.Reset();queue_.Reset();frequency_=0;
        return false;
    }
public:
    template<class Domain> bool Init(ID3D12Device* device,ID3D12CommandQueue* queue,Domain& domain) noexcept {
        if(attempted_)return heap_ && readback_ && queue_.Get()==queue;
        attempted_=true;
        // Optional diagnostics must not interrupt production, even when the
        // creation receipt itself cannot allocate. No commands reference these
        // objects until Init has succeeded and Begin has admitted a sample.
        try {
        if(!device || !queue || FAILED(queue->GetTimestampFrequency(&frequency_)) || !frequency_)return Disable();
        D3D12_QUERY_HEAP_DESC q{};q.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;q.Count=Slots*4;
        auto hr=device->CreateQueryHeap(&q,IID_PPV_ARGS(&heap_));
        if(!domain.Created("prep-timestamp-heap",hr,heap_.Get(),false))return Disable();
        D3D12_HEAP_PROPERTIES h{};h.Type=D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=Slots*4*sizeof(uint64_t);
        d.Height=d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        hr=device->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback_));
        if(!domain.Created("prep-timestamp-readback",hr,readback_.Get(),false))return Disable();
        queue_=queue;return true;
        } catch (...) {
            return Disable(); // attempted_ prevents repeated allocation on the hot path
        }
    }
    bool Poll(unsigned slot,ID3D12CommandQueue* queue,ID3D12Fence* expected,Reading& out) noexcept {
        if(slot>=Slots || queue_.Get()!=queue || !readback_)return false;
        auto& s=slots_[slot];
        if(!s.fence || s.fence.Get()!=expected || !s.value)return false;
        const auto completed=s.fence->GetCompletedValue();
        if(completed==UINT64_MAX || completed<s.value)return false;
        const SIZE_T begin=slot*4*sizeof(uint64_t),end=begin+4*sizeof(uint64_t);
        const D3D12_RANGE range{begin,end};void* data=nullptr;
        if(FAILED(readback_->Map(0,&range,&data)) || !data)return false;
        const auto* values=reinterpret_cast<const uint64_t*>(static_cast<const unsigned char*>(data)+begin);
        std::array<uint64_t,4> ticks{values[0],values[1],values[2],values[3]};
        const D3D12_RANGE none{0,0};readback_->Unmap(0,&none);
        out={};out.input=s.input;out.queue=reinterpret_cast<uintptr_t>(queue);out.fence=reinterpret_cast<uintptr_t>(expected);
        out.frequency=frequency_;out.target=s.value;out.completed=completed;out.known=frequency_ && ticks[0];
        for(unsigned i=0;i<3;++i){
            if(ticks[i+1]<ticks[i])out.known=false;
            else if(frequency_)out.milliseconds[i]=double(ticks[i+1]-ticks[i])*1000.0/double(frequency_);
        }
        s={};return true;
    }
    bool Begin(unsigned slot,ID3D12CommandQueue* queue,const Identity& input) noexcept {
        if(slot>=Slots || queue_.Get()!=queue || !heap_ || !readback_ || slots_[slot].fence)return false;
        auto& s=slots_[slot];s={};s.input=input;return true;
    }
    void Stamp(unsigned slot,unsigned boundary,ID3D12GraphicsCommandList* list) noexcept {
        if(slot>=Slots || boundary>=4 || !list)return;
        auto& s=slots_[slot];if(s.stamps!=boundary)return;
        list->EndQuery(heap_.Get(),D3D12_QUERY_TYPE_TIMESTAMP,slot*4+boundary);++s.stamps;
    }
    void Resolve(unsigned slot,ID3D12GraphicsCommandList* list) noexcept {
        if(slot>=Slots || !list || slots_[slot].stamps!=4)return;
        list->ResolveQueryData(heap_.Get(),D3D12_QUERY_TYPE_TIMESTAMP,slot*4,4,readback_.Get(),slot*4*sizeof(uint64_t));
        slots_[slot].resolved=true;
    }
    void Submitted(unsigned slot,ID3D12CommandQueue* queue,ID3D12Fence* fence,uint64_t value) noexcept {
        if(slot>=Slots || queue_.Get()!=queue || !fence || !value || value==UINT64_MAX)return;
        auto& s=slots_[slot];if(!s.resolved)return;
        s.fence=fence;s.value=value;s.resolved=false;
    }
};
}
