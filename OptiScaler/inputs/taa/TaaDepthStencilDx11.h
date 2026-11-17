// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d11.h>
#include <wrl/client.h>

namespace taa {
// A bound state alone does not say whether the draw tests any pixels: the
// actual OM attachment is part of its meaning. Read this afresh for each Probe
// and Begin; it is neither shader metadata nor a resource-retirement receipt.
struct DepthStencilDx11 {
    enum class Coverage { NoAttachment, Unconditional, DepthTest, StencilTest };
    bool bound=false,defaultState=false;
    UINT reference=0;
    D3D11_DEPTH_STENCIL_DESC desc{};
    static DepthStencilDx11 Capture(ID3D11DeviceContext* context,ID3D11DepthStencilView* view) {
        DepthStencilDx11 result;result.bound=view!=nullptr;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilState> state;
        context->OMGetDepthStencilState(&state,&result.reference);
        result.defaultState=!state;
        if(state)state->GetDesc(&result.desc);
        else {
            auto& d=result.desc;
            d.DepthEnable=TRUE;d.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;d.DepthFunc=D3D11_COMPARISON_LESS;
            d.StencilReadMask=D3D11_DEFAULT_STENCIL_READ_MASK;d.StencilWriteMask=D3D11_DEFAULT_STENCIL_WRITE_MASK;
            d.FrontFace={D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_COMPARISON_ALWAYS};
            d.BackFace=d.FrontFace;
        }
        return result;
    }
    Coverage Classify() const {
        if(!bound)return Coverage::NoAttachment;
        if(desc.DepthEnable&&desc.DepthFunc!=D3D11_COMPARISON_ALWAYS)return Coverage::DepthTest;
        if(desc.StencilEnable&&(desc.FrontFace.StencilFunc!=D3D11_COMPARISON_ALWAYS||desc.BackFace.StencilFunc!=D3D11_COMPARISON_ALWAYS))return Coverage::StencilTest;
        return Coverage::Unconditional;
    }
    const char* Reason() const {
        switch(Classify()) {
        case Coverage::DepthTest:return "depth-tested TAA draw";
        case Coverage::StencilTest:return "stencil-tested TAA draw";
        default:return nullptr;
        }
    }
    const char* Label() const {
        switch(Classify()) {
        case Coverage::NoAttachment:return "no_attachment";
        case Coverage::Unconditional:return "unconditional";
        case Coverage::DepthTest:return "depth_test";
        default:return "stencil_test";
        }
    }
};
} // namespace taa
