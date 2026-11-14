#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <utility>
#include "NativeObjectContracts.h"

// Device identity only. These queries grant no queue ordering, GPU completion,
// resource ownership or permission to replace an existing queue/fence epoch.
namespace D3D12DeviceIdentity
{
// Explicit native-object contracts used by HoYoShade/ReShade and Streamline.
// Unknown wrappers remain distinct; adapter LUIDs never identify a device.
inline constexpr GUID ReShadeNativeObject = NativeObjectContracts::ReShade;
inline constexpr GUID StreamlineNativeObject = NativeObjectContracts::Streamline;
inline constexpr size_t MaxProxyDepth = NativeObjectContracts::MaxDepth;

inline Microsoft::WRL::ComPtr<IUnknown> ComIdentity(IUnknown* object) noexcept
{
    Microsoft::WRL::ComPtr<IUnknown> result;
    if (!object || object->QueryInterface(IID_PPV_ARGS(&result)) != S_OK)
        result.Reset();
    return result;
}

inline Microsoft::WRL::ComPtr<IUnknown> NativeIdentity(IUnknown* object) noexcept
{
    using Microsoft::WRL::ComPtr;
    auto current = ComIdentity(object);
    std::array<ComPtr<IUnknown>, MaxProxyDepth + 1> visited;
    if (!current) return {};
    visited[0] = current;

    for (size_t depth = 0; depth <= MaxProxyDepth; ++depth)
    {
        ComPtr<IUnknown> next;
        for (const auto& iid : { ReShadeNativeObject, StreamlineNativeObject })
        {
            const auto hr = current->QueryInterface(iid, reinterpret_cast<void**>(next.GetAddressOf()));
            if (hr == E_NOINTERFACE && !next) continue;
            if (hr != S_OK || !next) return {};
            break;
        }
        if (!next)
        {
            // Only an actual D3D12 device can terminate a native-object chain.
            ComPtr<ID3D12Device> device;
            if (current.As(&device) != S_OK || !device) return {};
            auto identity = ComIdentity(device.Get());
            return identity.Get() == current.Get() ? identity : ComPtr<IUnknown> {};
        }
        if (depth == MaxProxyDepth) return {};
        next = ComIdentity(next.Get());
        if (!next) return {};
        for (size_t i = 0; i <= depth; ++i)
            if (visited[i].Get() == next.Get()) return {}; // self/cyclic proxy
        visited[depth + 1] = next;
        current = std::move(next);
    }
    return {};
}

inline bool Same(ID3D12Device* left, ID3D12Device* right) noexcept
{
    auto a = ComIdentity(left), b = ComIdentity(right);
    if (!a || !b) return false;
    if (a.Get() == b.Get()) return true;
    a = NativeIdentity(a.Get());
    b = NativeIdentity(b.Get());
    return a && b && a.Get() == b.Get();
}
}
