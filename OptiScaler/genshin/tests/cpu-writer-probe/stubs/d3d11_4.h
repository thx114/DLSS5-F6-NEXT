// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// CPU test boundary only: the production probe is compiled unchanged against
// these minimal COM interfaces. Real Windows IUnknown/WRL and thread IDs are
// used; no D3D runtime, device, SDK execution or GPU completion is simulated.
#include <Windows.h>
#include <Unknwn.h>
#include <climits>

enum D3D11_USAGE { D3D11_USAGE_DEFAULT, D3D11_USAGE_IMMUTABLE, D3D11_USAGE_DYNAMIC, D3D11_USAGE_STAGING };
enum D3D11_MAP { D3D11_MAP_READ = 1, D3D11_MAP_WRITE, D3D11_MAP_READ_WRITE, D3D11_MAP_WRITE_DISCARD, D3D11_MAP_WRITE_NO_OVERWRITE };
enum D3D11_DEVICE_CONTEXT_TYPE { D3D11_DEVICE_CONTEXT_IMMEDIATE, D3D11_DEVICE_CONTEXT_DEFERRED };
enum DXGI_FORMAT { DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R32_FLOAT = 41 };
enum D3D11_DSV_DIMENSION { D3D11_DSV_DIMENSION_UNKNOWN, D3D11_DSV_DIMENSION_TEXTURE2D = 3 };
enum D3D11_RTV_DIMENSION { D3D11_RTV_DIMENSION_UNKNOWN, D3D11_RTV_DIMENSION_TEXTURE2D = 4 };
inline constexpr UINT D3D11_BIND_CONSTANT_BUFFER = 4;
inline constexpr UINT D3D11_CPU_ACCESS_WRITE = 0x10000;
inline constexpr UINT D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT = 14;
inline constexpr UINT D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL = 0xffffffff;
struct D3D11_BUFFER_DESC {
    UINT ByteWidth = 0;
    D3D11_USAGE Usage = D3D11_USAGE_DEFAULT;
    UINT BindFlags = 0, CPUAccessFlags = 0, MiscFlags = 0, StructureByteStride = 0;
};
struct D3D11_TEXTURE2D_DESC { DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN; };
struct D3D11_BOX { UINT left, top, front, right, bottom, back; };
struct D3D11_MAPPED_SUBRESOURCE { void* pData = nullptr; UINT RowPitch = 0, DepthPitch = 0; };
struct D3D11_DEPTH_STENCIL_VIEW_DESC {
    D3D11_DSV_DIMENSION ViewDimension = D3D11_DSV_DIMENSION_UNKNOWN;
    struct { UINT MipSlice = 0; } Texture2D;
};
struct D3D11_RENDER_TARGET_VIEW_DESC {
    D3D11_RTV_DIMENSION ViewDimension = D3D11_RTV_DIMENSION_UNKNOWN;
    struct { UINT MipSlice = 0; } Texture2D;
};
MIDL_INTERFACE("11111111-1111-1111-1111-111111111101") ID3D11Device : IUnknown {};
MIDL_INTERFACE("11111111-1111-1111-1111-111111111102") ID3D11DeviceContext : IUnknown {
    virtual D3D11_DEVICE_CONTEXT_TYPE GetType() = 0;
    virtual void GetDevice(ID3D11Device**) = 0;
};
MIDL_INTERFACE("11111111-1111-1111-1111-111111111103") ID3D11Resource : IUnknown {
    virtual void GetDevice(ID3D11Device**) = 0;
};
MIDL_INTERFACE("11111111-1111-1111-1111-111111111104") ID3D11Buffer : ID3D11Resource {
    virtual void GetDesc(D3D11_BUFFER_DESC*) = 0;
};
MIDL_INTERFACE("11111111-1111-1111-1111-111111111105") ID3D11Texture2D : ID3D11Resource {
    virtual void GetDesc(D3D11_TEXTURE2D_DESC*) = 0;
};
MIDL_INTERFACE("11111111-1111-1111-1111-111111111106") ID3D11DepthStencilView : IUnknown {
    virtual void GetResource(ID3D11Resource**) = 0;
    virtual void GetDesc(D3D11_DEPTH_STENCIL_VIEW_DESC*) = 0;
};
MIDL_INTERFACE("11111111-1111-1111-1111-111111111107") ID3D11RenderTargetView : IUnknown {
    virtual void GetResource(ID3D11Resource**) = 0;
    virtual void GetDesc(D3D11_RENDER_TARGET_VIEW_DESC*) = 0;
};
