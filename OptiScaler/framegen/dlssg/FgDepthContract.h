#pragma once

#include <upscalers/GameRoutePolicy.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs.h>
#include <Util.h>
#include <climits>

// Input validation while the producer still owns the actual resources. These
// CPU facts do not certify GPU execution, depth pixels or generated frames.
namespace dlssg::depth_contract
{
inline GameRouting::Identity Game()
{
    static const auto game = GameRouting::IdentifyExecutable(Util::ExePath().filename().c_str());
    return game;
}

inline bool SameOwner(IUnknown* a, IUnknown* b)
{
    if (!a || !b) return false;
    Microsoft::WRL::ComPtr<IUnknown> x, y;
    return SUCCEEDED(a->QueryInterface(IID_PPV_ARGS(&x))) &&
        SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(&y))) && x && x.Get() == y.Get();
}

// Match the same physical list through Streamline's existing proxy interface.
// Keep the original COM owner too; this is identity resolution, not execution.
template<class T> inline Microsoft::WRL::ComPtr<T> NativeObject(T* list)
{
    Microsoft::WRL::ComPtr<T> result;
    if (!list) return result;
    static const GUID proxyId={0xADEC44E2,0x61F0,0x45C3,{0xAD,0x9F,0x1B,0x37,0x37,0x92,0x84,0xFF}};
    Microsoft::WRL::ComPtr<IUnknown> native;
    if (SUCCEEDED(list->QueryInterface(proxyId,reinterpret_cast<void**>(native.GetAddressOf()))) && native)
        native.As(&result);
    if (!result) result=list;
    return result;
}

inline bool OnDevice(ID3D12Device* device, ID3D12DeviceChild* child)
{
    Microsoft::WRL::ComPtr<ID3D12Device> actual;
    return device && child && SUCCEEDED(child->GetDevice(IID_PPV_ARGS(&actual))) &&
        SameOwner(device, actual.Get());
}

inline bool Region(const D3D12_RESOURCE_DESC& d, UINT left, UINT top, UINT64 width, UINT height)
{
    // Use subtraction so hostile/overflowing NGX extents cannot wrap into bounds.
    return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.DepthOrArraySize == 1 &&
        d.MipLevels && d.SampleDesc.Count == 1 && !d.SampleDesc.Quality &&
        d.Format != DXGI_FORMAT_UNKNOWN && d.Width<=UINT_MAX && width && height && width <= UINT_MAX &&
        left <= d.Width && top <= d.Height && width <= d.Width - left && height <= d.Height - top;
}

inline bool DepthFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R16_TYPELESS:case DXGI_FORMAT_R16_UNORM:case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R32_TYPELESS:case DXGI_FORMAT_R32_FLOAT:case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R24G8_TYPELESS:case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return true;
    default:return false;
    }
}

inline bool MotionFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R16G16_TYPELESS:case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R32G32_TYPELESS:case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return true;
    default:return false;
    }
}

inline void InputRect(const NVSDK_NGX_Parameter* parameters, bool depth, UINT& left, UINT& top)
{
    left = top = 0;
    parameters->Get(depth ? NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X :
                           NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, &left);
    parameters->Get(depth ? NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y :
                           NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, &top);
}
}
