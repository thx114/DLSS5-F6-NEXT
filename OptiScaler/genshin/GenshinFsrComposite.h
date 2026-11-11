// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

namespace GenshinFsrComposite {
// FEAC's colour operation is not part of the old accumulate profile. Replace
// the temporal upsampler with SR, then compose its colour at display resolution
// using the actual native t7/s1/CB0. The original native draw remains mandatory:
// it owns RTV0 and RTV1 alpha (including the new t3.z remapping). This pass only
// writes RGB. No old native colour/history is sampled or replayed.
inline constexpr char Source[]=R"(
Texture2D<float4> Upscaled:register(t0);
Texture2D<float4> Overlay:register(t7);
SamplerState NativeSampler:register(s1);
cbuffer NativeConstants:register(b0){float4 native[31];};
float4 vs(uint id:SV_VertexID):SV_Position {
    float2 p=float2((id<<1)&2,id&2);return float4(p*float2(2,-2)+float2(-1,1),0,1);
}
float4 ps(float4 position:SV_Position):SV_Target {
    uint2 pixel=(uint2)position.xy;
    float2 uv=(float2(pixel)+0.5)*native[27].zw-native[28].xy;
    float4 over=Overlay.SampleLevel(NativeSampler,uv,0);
    float3 base=Upscaled.Load(int3(pixel,0)).rgb;
    return float4(max(0.0,base+over.a*(over.rgb-base)),0);
})";
class Pass final {
    template<class T>using ComPtr=Microsoft::WRL::ComPtr<T>;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11BlendState> blend_;
    ComPtr<ID3D11DepthStencilState> depth_;
    ComPtr<ID3D11RasterizerState> raster_;
public:
    bool Init(ID3D11Device* device) noexcept {
        if(vs_ && ps_ && blend_ && depth_ && raster_)return true;
        if(!device)return false;
        ComPtr<ID3DBlob> vs,ps,error;
        if(FAILED(D3DCompile(Source,sizeof(Source)-1,"fsr_native_overlay",nullptr,nullptr,"vs","vs_5_0",
            D3DCOMPILE_ENABLE_STRICTNESS,0,&vs,&error)) ||
           FAILED(D3DCompile(Source,sizeof(Source)-1,"fsr_native_overlay",nullptr,nullptr,"ps","ps_5_0",
            D3DCOMPILE_ENABLE_STRICTNESS,0,&ps,&error)) ||
           FAILED(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&vs_)) ||
           FAILED(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&ps_)))return false;
        D3D11_BLEND_DESC b{};
        b.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_RED|D3D11_COLOR_WRITE_ENABLE_GREEN|D3D11_COLOR_WRITE_ENABLE_BLUE;
        D3D11_DEPTH_STENCIL_DESC d{};
        D3D11_RASTERIZER_DESC r{};r.FillMode=D3D11_FILL_SOLID;r.CullMode=D3D11_CULL_NONE;r.DepthClipEnable=TRUE;
        return SUCCEEDED(device->CreateBlendState(&b,&blend_)) &&
            SUCCEEDED(device->CreateDepthStencilState(&d,&depth_)) &&
            SUCCEEDED(device->CreateRasterizerState(&r,&raster_));
    }
    // Caller owns an active private context-state scope. All views, CB and
    // sampler are retained by that invocation's real completion-query Flight.
    void Draw(ID3D11DeviceContext* context,ID3D11ShaderResourceView* upscaled,
        ID3D11ShaderResourceView* overlay,ID3D11SamplerState* sampler,ID3D11Buffer* constants,
        ID3D11RenderTargetView* target,UINT width,UINT height) const noexcept {
        context->SetPredication(nullptr,FALSE);
        context->SOSetTargets(0,nullptr,nullptr);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs_.Get(),nullptr,0);context->PSSetShader(ps_.Get(),nullptr,0);
        context->GSSetShader(nullptr,nullptr,0);context->HSSetShader(nullptr,nullptr,0);context->DSSetShader(nullptr,nullptr,0);
        context->OMSetRenderTargets(1,&target,nullptr);
        context->OMSetBlendState(blend_.Get(),nullptr,UINT_MAX);context->OMSetDepthStencilState(depth_.Get(),0);
        const D3D11_VIEWPORT viewport{0,0,static_cast<float>(width),static_cast<float>(height),0,1};
        context->RSSetState(raster_.Get());context->RSSetViewports(1,&viewport);
        context->PSSetShaderResources(0,1,&upscaled);context->PSSetShaderResources(7,1,&overlay);
        context->PSSetSamplers(1,1,&sampler);context->PSSetConstantBuffers(0,1,&constants);
        context->Draw(3,0);
        ID3D11ShaderResourceView* empty=nullptr;
        context->PSSetShaderResources(0,1,&empty);context->PSSetShaderResources(7,1,&empty);
        context->OMSetRenderTargets(0,nullptr,nullptr);
    }
};
}
