// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d11.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <utility>

namespace FramePipeline {
// Imports an existing allocation; never copies pixels or establishes readiness.
// The creating caller must supply the handle it exported from this exact source;
// DXGI permits only one CreateSharedHandle call per resource. The handle is
// borrowed for the initial import only. The caller owns ordering and retention
// through the real reader fence. Unsupported imports are remembered per owner.
class Dx11SharedRead final {
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D11Texture2D> source_;
    Ptr<ID3D12Device> device_;
    Ptr<ID3D12Resource> resource_;
    UINT width_=0,height_=0;
    DXGI_FORMAT format_=DXGI_FORMAT_UNKNOWN;
    bool attempted_=false;
    static bool Same(IUnknown* a,IUnknown* b) noexcept {
        Ptr<IUnknown> x,y;
        return a&&b&&SUCCEEDED(a->QueryInterface(IID_PPV_ARGS(&x)))&&
            SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(&y)))&&x&&x==y;
    }
public:
    ID3D12Resource* Open(ID3D11Texture2D* source,ID3D12Device* device,
                        UINT width,UINT height,DXGI_FORMAT format,HANDLE handle=nullptr) noexcept {
        if(!source||!device||!width||!height)return nullptr;
        if(attempted_)return (source==source_.Get()||Same(source,source_.Get()))&&
            (device==device_.Get()||Same(device,device_.Get()))&&resource_&&
            width_==width&&height_==height&&format_==format&&
            SUCCEEDED(device->GetDeviceRemovedReason())?resource_.Get():nullptr;
        if(!handle||handle==INVALID_HANDLE_VALUE)return nullptr;
        source_=source;device_=device;width_=width;height_=height;format_=format;attempted_=true;
        D3D11_TEXTURE2D_DESC d{};source->GetDesc(&d);
        if(d.Width!=width||d.Height!=height||d.Format!=format||d.MipLevels!=1||d.ArraySize!=1||
           d.SampleDesc.Count!=1||d.SampleDesc.Quality||d.Usage!=D3D11_USAGE_DEFAULT||d.CPUAccessFlags||
           !(d.BindFlags&D3D11_BIND_SHADER_RESOURCE)||!(d.MiscFlags&D3D11_RESOURCE_MISC_SHARED_NTHANDLE))return nullptr;
        Ptr<ID3D12Resource> opened;Ptr<ID3D12Device> actual;
        if(FAILED(device->OpenSharedHandle(handle,IID_PPV_ARGS(&opened)))||!opened||
           FAILED(opened->GetDevice(IID_PPV_ARGS(&actual)))||!Same(actual.Get(),device)||
           FAILED(device->GetDeviceRemovedReason()))return nullptr;
        const auto r=opened->GetDesc();
        if(r.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||r.Width!=width||r.Height!=height||r.Format!=format||
           r.DepthOrArraySize!=1||r.MipLevels!=1||r.SampleDesc.Count!=1||r.SampleDesc.Quality||
           (r.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)||!(r.Flags&D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS))return nullptr;
        resource_=std::move(opened);return resource_.Get();
    }
};
} // namespace FramePipeline
