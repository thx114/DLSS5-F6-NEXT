#pragma once
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

namespace ProviderOutputQueue
{
inline Microsoft::WRL::ComPtr<IUnknown> Identity(IUnknown* object)
{
    Microsoft::WRL::ComPtr<IUnknown> identity;
    if (object) object->QueryInterface(IID_PPV_ARGS(&identity));
    return identity;
}

// The overlay has one ordered timeline. Authenticate every current buffer;
// a split presenting queue cannot inherit write permission from buffer zero.
inline Microsoft::WRL::ComPtr<IUnknown> AfterResize(IDXGISwapChain* chain, UINT count,
    const UINT* nodes, IUnknown* const* queues)
{
    using Microsoft::WRL::ComPtr;
    ComPtr<IUnknown> selected;
    DXGI_SWAP_CHAIN_DESC desc {};
    ComPtr<ID3D12Device> owner;
    if (!chain || !queues || !nodes || FAILED(chain->GetDesc(&desc)) || !desc.BufferCount ||
        (count && count != desc.BufferCount) || FAILED(chain->GetDevice(IID_PPV_ARGS(&owner)))) return {};
    const auto deviceIdentity = Identity(owner.Get());
    if (!deviceIdentity) return {};
    for (UINT i = 0; i < desc.BufferCount; ++i)
    {
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12Device> device;
        if (!queues[i] || FAILED(queues[i]->QueryInterface(IID_PPV_ARGS(&queue))) ||
            queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT ||
            FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) || Identity(device.Get()) != deviceIdentity ||
            (nodes[i] && queue->GetDesc().NodeMask && nodes[i] != queue->GetDesc().NodeMask)) return {};
        const auto identity = Identity(queue.Get());
        if (!identity || (selected && identity != Identity(selected.Get()))) return {};
        selected = queue;
    }
    return selected;
}
}
