// SPDX-License-Identifier: GPL-3.0-or-later
#define NOMINMAX
#include "../GenshinFsrComposite.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
using Microsoft::WRL::ComPtr;
namespace {
unsigned checks=0;
void Check(bool ok,const char* why){++checks;if(!ok){std::fprintf(stderr,"FAIL %s\n",why);std::exit(1);}}
void Hr(HRESULT hr,const char* why){Check(SUCCEEDED(hr),why);}
using Pixel=std::array<float,4>;
constexpr UINT W=4,H=2;
}
int main(){
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    Hr(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"WARP");
    GenshinFsrComposite::Pass pass;Check(pass.Init(device.Get()),"real production overlay shaders compile/create");
    std::array<Pixel,W*H> base{},over{},native{};
    for(UINT i=0;i<W*H;++i){base[i]={.1f*float(i),.8f,.2f,1};over[i]={1,.05f*float(i),.4f,float(i%4)*.5f};native[i]={.9f,.9f,.9f,.1f+.03f*float(i)};}
    const auto texture=[&](const auto& pixels,UINT bind){
        D3D11_TEXTURE2D_DESC d{};d.Width=W;d.Height=H;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
        d.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;d.BindFlags=bind;
        D3D11_SUBRESOURCE_DATA data{};data.pSysMem=pixels.data();data.SysMemPitch=sizeof(Pixel)*W;
        ComPtr<ID3D11Texture2D> t;Hr(device->CreateTexture2D(&d,&data,&t),"texture");return t;
    };
    auto source=texture(base,D3D11_BIND_SHADER_RESOURCE),overlay=texture(over,D3D11_BIND_SHADER_RESOURCE);
    auto target=texture(native,D3D11_BIND_RENDER_TARGET),companion=texture(native,D3D11_BIND_RENDER_TARGET);
    ComPtr<ID3D11ShaderResourceView> sourceView,overlayView;
    Hr(device->CreateShaderResourceView(source.Get(),nullptr,&sourceView),"SR view");
    Hr(device->CreateShaderResourceView(overlay.Get(),nullptr,&overlayView),"real t7 view");
    ComPtr<ID3D11RenderTargetView> targetView,companionView;
    Hr(device->CreateRenderTargetView(target.Get(),nullptr,&targetView),"target");
    Hr(device->CreateRenderTargetView(companion.Get(),nullptr,&companionView),"companion");
    D3D11_TEXTURE2D_DESC rd{};target->GetDesc(&rd);rd.BindFlags=0;rd.Usage=D3D11_USAGE_STAGING;rd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> readback;Hr(device->CreateTexture2D(&rd,nullptr,&readback),"readback");
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=31*16;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> constants;Hr(device->CreateBuffer(&bd,nullptr,&constants),"actual CB0 size");
    D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;Hr(device->CreateSamplerState(&sd,&sampler),"s1");
    for(int offset:{0,1,-1,5}) {
        std::array<Pixel,31> cb{};cb[27][2]=1.f/W;cb[27][3]=1.f/H;cb[28][0]=float(offset)/W;
        context->UpdateSubresource(constants.Get(),0,nullptr,cb.data(),0,0);
        context->UpdateSubresource(target.Get(),0,nullptr,native.data(),sizeof(Pixel)*W,0);
        // A real native MRT write is represented by initialized target alpha /
        // companion bytes; this fixture does not certify game token admission.
        ID3D11RenderTargetView* mrt[]{companionView.Get(),targetView.Get()};context->OMSetRenderTargets(2,mrt,nullptr);
        pass.Draw(context.Get(),sourceView.Get(),overlayView.Get(),sampler.Get(),constants.Get(),targetView.Get(),W,H);
        context->CopyResource(readback.Get(),target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};Hr(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped),"completed WARP RGB readback");
        for(UINT y=0;y<H;++y)for(UINT x=0;x<W;++x){
            const auto& got=reinterpret_cast<const Pixel*>(static_cast<const char*>(mapped.pData)+y*mapped.RowPitch)[x];
            const auto& b=base[y*W+x];const auto& o=over[y*W+UINT(std::clamp(int(x)-offset,0,int(W)-1))];
            for(unsigned c=0;c<3;++c)Check(std::abs(got[c]-std::max(0.f,b[c]+o[3]*(o[c]-b[c])))<.00001f,"native t7/s1/CB colour formula");
            Check(got[3]==native[y*W+x][3],"native alpha preserved exactly");
        }
        context->Unmap(readback.Get(),0);
        context->CopyResource(readback.Get(),companion.Get());Hr(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped),"companion read");
        for(UINT y=0;y<H;++y)for(UINT x=0;x<W;++x){
            const auto& got=reinterpret_cast<const Pixel*>(static_cast<const char*>(mapped.pData)+y*mapped.RowPitch)[x];
            Check(got==native[y*W+x],"companion untouched");
        }
        context->Unmap(readback.Get(),0);
    }
    context->ClearState();std::printf("FsrComposite: %u WARP checks; production shader RGB/alpha/companion; no game or SDK proof\n",checks);
}
