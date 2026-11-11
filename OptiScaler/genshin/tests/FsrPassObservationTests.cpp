// SPDX-License-Identifier: GPL-3.0-or-later
#include "../GenshinFsrPassObservation.h"
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

using Microsoft::WRL::ComPtr;
using namespace GenshinLegacy::FsrPassObservation;
int main(int argc,char** argv) {
    if(argc==2){
        SetLastError(0x5321);const bool requested=ProcessRequested();
        if(requested!=(argv[1][0]=='1') || GetLastError()!=0x5321)return 1;
        std::puts("FSR observation process switch: exact value and Win32 error preserved");return 0;
    }
    unsigned checks=0;
    const auto require=[&](bool ok,const char* message){++checks;if(!ok){std::fprintf(stderr,"FSR observation: %s\n",message);std::exit(1);}};
    const ScanBudget::Owner owner{1,2,3,4,5,6,7,8};
    SetEnvironmentVariableW(L"OPTISCALER_FSR_PASS_OBSERVATION",nullptr);SetLastError(0x5321);
    require(!ProcessRequested(),"normal gameplay enabled missing-pass collection");
    require(GetLastError()==0x5321,"process switch changed Win32 error");
    PendingDelivery delivery;
    require(!delivery.TryEmit(owner,{},owner.thread),"invalid Present consumed pending record");
    auto otherOwner=owner;++otherOwner.instance;
    require(!delivery.TryEmit(owner,otherOwner,owner.thread),"foreign owner consumed pending record");
    require(!delivery.TryEmit(owner,owner,owner.thread+1),"foreign thread consumed pending record");
    require(delivery.TryEmit(owner,owner,owner.thread),"restored owner lost pending record");
    require(!delivery.TryEmit(owner,owner,owner.thread),"record emitted twice");
    delivery={};
    require(!delivery.TryEmit({}, {},owner.thread),"unknown expected owner emitted record");
    require(delivery.TryEmit(otherOwner,otherOwner,otherOwner.thread),"replacement record not deliverable");
    ScanBudget scan;
    scan.Advance(owner,100,0,true);require(!scan.Active(owner),"no real producer opened scan");
    scan.Advance(owner,103,100,true);require(!scan.Active(owner),"scan opened before gap");
    scan.Advance(owner,104,100,true);require(scan.Active(owner),"stalled window missing");
    require(!scan.Ends(owner,104),"same render closed scan");
    auto wrong=owner;++wrong.configEpoch;require(!scan.Spend(wrong),"other controls consumed window");
    unsigned offers=0;for(unsigned i=0;i<ScanBudget::MaxDraws+2;++i)if(scan.Spend(owner))++offers;
    require(offers==ScanBudget::MaxDraws,"draw budget ended early or unbounded");
    require(!scan.Spend(owner),"draw budget unbounded");require(scan.Ends(owner,105),"next boundary did not close");
    scan.Advance(owner,105,105,false);require(!scan.Active(owner),"recovery kept scan active");
    scan.Advance(owner,109,105,true);require(!scan.Active(owner),"recovery replenished offset allowance");
    for(uint64_t gap: {64ull,256ull,512ull}) {
        scan.Advance(owner,105+gap,105,true);require(scan.Active(owner),"later stalled window missing");
        require(scan.Spend(owner),"later window no work");scan.Advance(owner,106+gap,105,false);
    }
    scan.Advance(owner,10000,105,true);require(!scan.Active(owner),"owner window allowance unbounded");
    scan.Advance({},0,0,false);scan.Advance(owner,10001,105,true);
    require(!scan.Active(owner),"unknown interval replenished same owner budget");
    scan.Advance(wrong,110,105,true);require(scan.Active(wrong),"new exact owner did not reset window");
    scan.Advance({},0,0,false);require(!scan.Active(wrong),"invalid owner retained scan");

    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL level{};
    require(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,&level,&context)),"WARP device");
    std::array<ComPtr<ID3D11ShaderResourceView>,7> srvs;
    std::array<ComPtr<ID3D11RenderTargetView>,2> rtvs;
    const auto texture=[&](UINT width,UINT height,UINT bind) {
        D3D11_TEXTURE2D_DESC d{};d.Width=width;d.Height=height;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
        d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=bind;
        ComPtr<ID3D11Texture2D> result;require(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&result)),"texture");return result;
    };
    for(unsigned i=0;i<7;++i){const UINT w=i==1?16:i>=5?64:32,h=i==1?16:i>=5?32:16;
        auto t=texture(w,h,D3D11_BIND_SHADER_RESOURCE);require(SUCCEEDED(device->CreateShaderResourceView(t.Get(),nullptr,&srvs[i])),"srv");}
    for(auto& rtv:rtvs){auto t=texture(64,32,D3D11_BIND_RENDER_TARGET);require(SUCCEEDED(device->CreateRenderTargetView(t.Get(),nullptr,&rtv)),"rtv");}
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=464;bd.Usage=D3D11_USAGE_DEFAULT;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> cb;require(SUCCEEDED(device->CreateBuffer(&bd,nullptr,&cb)),"cb");
    std::array<ID3D11ShaderResourceView*,7> inputs{};for(unsigned i=0;i<7;++i)inputs[i]=srvs[i].Get();
    context->PSSetShaderResources(0,7,inputs.data());auto* rawCb=cb.Get();context->PSSetConstantBuffers(0,1,&rawCb);
    D3D11_VIEWPORT viewport{0,0,64,32,0,1};context->RSSetViewports(1,&viewport);
    std::array<ID3D11RenderTargetView*,2> outputs{rtvs[0].Get(),rtvs[1].Get()};
    context->OMSetRenderTargets(1,outputs.data(),nullptr);require(!Inspect(context.Get()).dual,"single RTV not prescreened");
    context->OMSetRenderTargets(2,outputs.data(),nullptr);
    const auto facts=Inspect(context.Get());require(facts.dual && facts.Failures()==0,"actual bound seven-input signature missed");
    require(facts.inputs[0].width==32 && facts.outputs[1].width==64 && facts.cbBytes==464,"actual facts lost");
    // Getters must not mutate, bind, draw or manufacture a shader identity.
    ComPtr<ID3D11RenderTargetView> after[2];ID3D11RenderTargetView* rawAfter[2]{};
    context->OMGetRenderTargets(2,rawAfter,nullptr);for(unsigned i=0;i<2;++i){after[i].Attach(rawAfter[i]);require(after[i].Get()==outputs[i],"inspector changed outputs");}
    for(unsigned bit=0;bit<7;++bit){auto bad=facts;
        switch(bit){case 0:bad.inputs[6]={};break;case 1:bad.cbBytes=448;break;case 2:bad.inputs[1].width=15;break;
        case 3:bad.inputs[3].width=31;break;case 4:bad.inputs[5].height=31;break;case 5:bad.viewport.TopLeftX=1;break;
        case 6:bad.inputs[0].width=128;break;}
        require((bad.Failures()&(1u<<bit))!=0,"partial signature refusal missing");}
    context->ClearState();require(Inspect(context.Get()).Failures()!=0,"ClearState manufactured signature");
    std::printf("FSR pass observation: %u checks; real WARP bindings; no draw, source ticket or SDK permission\n",checks);
}
