// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <utility>
#include <array>

namespace GenshinTransport {
// Public object ownership only. No private IID, unwrap, same-adapter or
// canonical-IUnknown substitution can authorize a different submission domain.
struct GpuWitness {
    const char* reason="ok";
    HRESULT hr=S_OK;
    Microsoft::WRL::ComPtr<ID3D12Device> actual;
    explicit operator bool() const noexcept {return SUCCEEDED(hr);}
    bool Reject(const char* why,HRESULT result=E_INVALIDARG) noexcept {
        reason=why;hr=FAILED(result)?result:E_INVALIDARG;return false;
    }
    bool Owns(ID3D12Device* expected,ID3D12DeviceChild* child,const char* why) noexcept {
        actual.Reset();
        if(!expected || !child)return Reject(why,E_POINTER);
        const auto result=child->GetDevice(IID_PPV_ARGS(&actual));
        if(FAILED(result) || !actual || actual.Get()!=expected)return Reject(why,result);
        return true;
    }
    bool Commands(ID3D12Device* device,ID3D12CommandQueue* queue,
        ID3D12GraphicsCommandList* list,ID3D12CommandAllocator* allocator,ID3D12Fence* fence) noexcept {
        if(!Owns(device,queue,"queue-device"))return false;
        if(queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT)return Reject("queue-type");
        if(!Owns(device,list,"list-device"))return false;
        if(list->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT)return Reject("list-type");
        return Owns(device,allocator,"allocator-device") && Owns(device,fence,"fence-device");
    }
};

// Creation and public ownership are different roles. The reference implementation
// forwards allocators/fences, wraps queue/list/heaps, and redirects resource
// GetDevice. This does not prove the loaded private core ABI: actual creation
// receipts below must match each exact public owner before any submission.
// An allocator's native owner must NEVER become the subsequent creation entry.
// No pointer alias relation is introduced: each role and returned object is exact.
class GpuWorkDomain final {
    Microsoft::WRL::ComPtr<ID3D12Device> creator_,publicOwner_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heapProbe_;
    struct Recording {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    };
public:
    static constexpr size_t RecordingCount=3;
private:
    std::array<Recording,RecordingCount> recordings_{};
public:
    ID3D12Device* Creator() const noexcept {return creator_.Get();}
    ID3D12Device* PublicOwner() const noexcept {return publicOwner_.Get();}
    ID3D12CommandQueue* Queue() const noexcept {return queue_.Get();}
    ID3D12DescriptorHeap* HeapProbe() const noexcept {return heapProbe_.Get();}
    bool BoundQueue(ID3D12CommandQueue* queue,GpuWitness& witness) const noexcept {
        if(!queue_ || queue!=queue_.Get())return witness.Reject("queue-binding");
        if(!witness.Owns(Creator(),queue,"queue-device"))return false;
        return queue->GetDesc().Type==D3D12_COMMAND_LIST_TYPE_DIRECT || witness.Reject("queue-type");
    }
    bool Create(ID3D12Device* creator,GpuWitness& witness) {
        if(creator_ || publicOwner_ || queue_)return witness.Reject("domain-already-created");
        if(!creator)return witness.Reject("creator-missing",E_POINTER);
        Microsoft::WRL::ComPtr<ID3D12Device> creationPin=creator;
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> anchor;
        auto hr=creator->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&anchor));
        if(FAILED(hr) || !anchor)return witness.Reject("anchor-create",hr);
        Microsoft::WRL::ComPtr<ID3D12Device> owner;
        hr=anchor->GetDevice(IID_PPV_ARGS(&owner));
        if(FAILED(hr) || !owner)return witness.Reject("anchor-device",hr);
        hr=creator->GetDeviceRemovedReason();
        if(FAILED(hr))return witness.Reject("creator-removed",hr);
        hr=owner->GetDeviceRemovedReason();
        if(FAILED(hr))return witness.Reject("public-owner-removed",hr);
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
        D3D12_COMMAND_QUEUE_DESC desc{};desc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr=creator->CreateCommandQueue(&desc,IID_PPV_ARGS(&queue));
        if(FAILED(hr) || !queue)return witness.Reject("work-queue-create",hr);
        if(!witness.Owns(creator,queue.Get(),"work-queue-device"))return false;
        if(queue->GetDesc().Type!=desc.Type)return witness.Reject("work-queue-type");
        // A real shader-visible heap creation receipt tests the owner that NGX's
        // heaps must expose. This is not a private-layout or module-name probe.
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors=1;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr=creator->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap));
        if(FAILED(hr) || !heap)return witness.Reject("work-heap-create",hr);
        if(!witness.Owns(creator,heap.Get(),"work-heap-device"))return false;
        creator_=std::move(creationPin);publicOwner_=std::move(owner);
        queue_=std::move(queue);heapProbe_=std::move(heap);return true;
    }
    bool CreateRecording(size_t slot,ID3D12CommandAllocator** allocator,
        ID3D12GraphicsCommandList** list,GpuWitness& witness) {
        if(!Creator() || !allocator || !list || slot>=recordings_.size())return witness.Reject("recording-arguments");
        if(*allocator || *list)return witness.Reject("recording-output-not-empty");
        auto& receipt=recordings_[slot];
        if(receipt.allocator || receipt.list)return witness.Reject("recording-already-created");
        Recording created;
        auto hr=Creator()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&created.allocator));
        if(FAILED(hr) || !created.allocator)return witness.Reject("recording-allocator-create",hr);
        if(!witness.Owns(PublicOwner(),created.allocator.Get(),"recording-allocator-device"))return false;
        hr=Creator()->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,created.allocator.Get(),nullptr,
            IID_PPV_ARGS(&created.list));
        if(FAILED(hr) || !created.list)return witness.Reject("recording-list-create",hr);
        if(!witness.Owns(Creator(),created.list.Get(),"recording-list-device"))return false;
        if(created.list->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT)return witness.Reject("recording-list-type");
        hr=created.list->Close();
        if(FAILED(hr))return witness.Reject("recording-close",hr);
        // Publish only the exact successfully created pair, retaining both.
        receipt=std::move(created);
        *allocator=receipt.allocator.Get();(*allocator)->AddRef();
        *list=receipt.list.Get();(*list)->AddRef();return true;
    }
    // Only the feature may clear receipts, after its existing GPU drain gate.
    // Leases/quarantines keep independent strong copies of the old receipts.
    void RetireRecordings() noexcept {recordings_={};}
    bool Commands(ID3D12CommandQueue* queue,ID3D12GraphicsCommandList* list,
        ID3D12CommandAllocator* allocator,ID3D12Fence* fence,GpuWitness& witness) const noexcept {
        if(!BoundQueue(queue,witness))return false;
        bool bound=false;
        for(const auto& receipt:recordings_)
            if(list && allocator && list==receipt.list.Get() && allocator==receipt.allocator.Get()){bound=true;break;}
        if(!bound)return witness.Reject("recording-binding");
        if(!witness.Owns(Creator(),list,"list-device"))return false;
        if(list->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT)return witness.Reject("list-type");
        return witness.Owns(PublicOwner(),allocator,"allocator-device") &&
            witness.Owns(PublicOwner(),fence,"fence-device");
    }
};
}
