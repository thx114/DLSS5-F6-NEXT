// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <runtime/NativeObjectContracts.h>
#include <unknwn.h>
#include <wrl/client.h>
#include <array>
#include <utility>

namespace D3D11On12FactoryBoundary
{
inline thread_local unsigned depth = 0;
// On12 is a distinct creation operation even when called inside a D3D11
// factory. Inherit its nested-return observer without sharing policy depth.
template<class DeviceFactoryDomain> struct Domain : DeviceFactoryDomain {};
class Scope final
{
public:
    Scope() noexcept { ++depth; }
    ~Scope() { --depth; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

inline bool Internal(UINT flags) noexcept
{
    // Observed Windows D3D11On12 internal-factory flag, not a public DXGI flag.
    // Neither this bit alone nor a different thread's device creation opts in.
    return depth != 0 && (flags & 0x80000000u) != 0;
}

// A successful creation returns one owned interface reference. Replace that
// reference transactionally, preserving the requested IID rather than casting
// an IUnknown or a higher factory interface to the requested interface.
inline HRESULT Returned(HRESULT result, UINT flags, REFIID iid, void** output) noexcept
{
    if (!Internal(flags) || FAILED(result)) return result;
    if (!output || !*output) return E_UNEXPECTED;
    using Microsoft::WRL::ComPtr;
    ComPtr<IUnknown> current;
    current.Attach(static_cast<IUnknown*>(*output));
    *output = nullptr;
    std::array<ComPtr<IUnknown>, NativeObjectContracts::MaxDepth + 1> visited;
    if (current.As(&visited[0]) != S_OK || !visited[0]) return E_NOINTERFACE;
    for (size_t index = 0; index <= NativeObjectContracts::MaxDepth; ++index)
    {
        ComPtr<IUnknown> next;
        for (const auto& contract : { NativeObjectContracts::ReShade, NativeObjectContracts::Streamline })
        {
            const auto hr = current->QueryInterface(contract, reinterpret_cast<void**>(next.GetAddressOf()));
            if (hr == E_NOINTERFACE && !next) continue;
            if (hr != S_OK || !next) return FAILED(hr) ? hr : E_NOINTERFACE;
            break;
        }
        if (!next)
        {
            void* requested = nullptr;
            const auto hr = current->QueryInterface(iid, &requested);
            if (hr != S_OK || !requested)
            {
                if (requested) static_cast<IUnknown*>(requested)->Release();
                return FAILED(hr) ? hr : E_NOINTERFACE;
            }
            *output = requested;
            return result;
        }
        if (index == NativeObjectContracts::MaxDepth) return E_NOINTERFACE;
        if (next.As(&visited[index + 1]) != S_OK || !visited[index + 1]) return E_NOINTERFACE;
        for (size_t previous = 0; previous <= index; ++previous)
            if (visited[previous].Get() == visited[index + 1].Get()) return E_NOINTERFACE;
        current = std::move(next);
    }
    return E_NOINTERFACE;
}
}
