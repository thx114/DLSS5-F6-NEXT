// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d12.h>
#include <wrl/client.h>

namespace FramePipeline {
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


}
