#pragma once

// CPU-only surface, intentionally not a binary D3D12 vtable replacement.
// IUnknown, REFIID, HRESULT, IID_PPV_ARGS and WRL remain Windows implementations.
// These UUIDs and the signatures below match the Windows D3D12 interfaces.
// No D3D12 runtime, device factory, adapter enumeration or GPU is used.
#include <windows.h>
#include <unknwn.h>

enum D3D12_FENCE_FLAGS { D3D12_FENCE_FLAG_NONE = 0 };

MIDL_INTERFACE("0a753dcf-c4d8-4b91-adf6-be5a60d95a76")
ID3D12Fence : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** device) = 0;
    virtual UINT64 STDMETHODCALLTYPE GetCompletedValue() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEventOnCompletion(UINT64 value, HANDLE event) = 0;
};

MIDL_INTERFACE("189819f1-1db6-4b57-be54-1821339b85f7")
ID3D12Device : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE CreateFence(UINT64 initialValue, D3D12_FENCE_FLAGS flags,
                                                 REFIID riid, void** fence) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() = 0;
    virtual LUID STDMETHODCALLTYPE GetAdapterLuid() = 0;
};

MIDL_INTERFACE("0ec870a6-5d7e-4c22-8cfc-5baae07616ed")
ID3D12CommandQueue : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** device) = 0;
    virtual HRESULT STDMETHODCALLTYPE Signal(ID3D12Fence* fence, UINT64 value) = 0;
};
