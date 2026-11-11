// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d11.h>
#include <cfloat>
#include <cstdint>
#include <cstring>

namespace GenshinDepthMapping::SamplerRecipe {
// PSSetSamplers permits a null binding and defines its effective state.
// Keep this API default distinct from an absent/unknown captured binding.
inline D3D11_SAMPLER_DESC Effective(ID3D11SamplerState* sampler)noexcept {
    D3D11_SAMPLER_DESC d{};
    if(sampler){sampler->GetDesc(&d);return d;}
    d.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    d.AddressU=d.AddressV=d.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
    d.MaxAnisotropy=1;d.ComparisonFunc=D3D11_COMPARISON_NEVER;
    for(auto& value:d.BorderColor)value=1.0f;
    d.MinLOD=-FLT_MAX;d.MaxLOD=FLT_MAX;
    return d;
}
inline bool CenterEnvelope(const D3D11_SAMPLER_DESC& d,UINT mipLevels)noexcept {
    const auto finite=[](float value)noexcept {uint32_t bits=0;std::memcpy(&bits,&value,4);return (bits&0x7f800000u)!=0x7f800000u;};
    // One actual texture mip and a mip-zero view are independently checked by
    // the caller. Mip interpolation then has no second level to blend. Requiring
    // equal MIN/MAG modes avoids guessing implicit-derivative LOD selection.
    if(mipLevels!=1 || !finite(d.MipLODBias) || !finite(d.MinLOD) ||
       !finite(d.MaxLOD) || d.MinLOD>d.MaxLOD || d.MipLODBias<D3D11_MIP_LOD_BIAS_MIN ||
       d.MipLODBias>D3D11_MIP_LOD_BIAS_MAX || d.AddressU!=D3D11_TEXTURE_ADDRESS_CLAMP ||
       d.AddressV!=D3D11_TEXTURE_ADDRESS_CLAMP)return false;
    return true;
}
inline bool Classify(const D3D11_SAMPLER_DESC& d,UINT mipLevels,UINT& linear)noexcept {
    linear=0;if(!CenterEnvelope(d,mipLevels))return false;
    switch(d.Filter){
    case D3D11_FILTER_MIN_MAG_MIP_POINT:
    case D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR:return true;
    case D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT:
    case D3D11_FILTER_MIN_MAG_MIP_LINEAR:linear=1;return true;
    default:return false; // mixed, anisotropic, comparison and reduction modes
    }
}
// Only for the exact retained Scene PS/VS with actual qualified replay inputs.
// SceneUv writes the interpolated UV center without sampling the colour plane.
// This is not anisotropic colour-filter emulation; Compose must use Classify.
inline bool ClassifySceneCenter(const D3D11_SAMPLER_DESC& d,UINT mipLevels,UINT& linear)noexcept {
    if(Classify(d,mipLevels,linear))return true;
    linear=0;
    return CenterEnvelope(d,mipLevels) && d.Filter==D3D11_FILTER_ANISOTROPIC &&
        d.MaxAnisotropy>=1 && d.MaxAnisotropy<=D3D11_MAX_MAXANISOTROPY;
}
}
