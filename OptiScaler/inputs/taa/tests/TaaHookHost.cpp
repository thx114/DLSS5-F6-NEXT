// Isolated graphics host: runs the actual DLL's shader/draw/dispatch hooks and
// selected NGX provider. No game, installed profile, launcher or ASI is touched.
#include <windows.h>
#include <d3d11_1.h>
#include <d3d11sdklayers.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <resource_tracking/DrawObservation_Dx11.h>
#include "TaaProjectionFixture.h"
#include "../TaaFullscreenDx11.h"
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <stdexcept>
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <tuple>
#include <regex>
#include <map>
#include <source_location>

using Microsoft::WRL::ComPtr;
namespace {
void Check(bool ok,const char* label) {if(!ok)throw std::runtime_error(label);}
ID3D11Device* fixtureDevice=nullptr;
void Hr(HRESULT result,const std::source_location at=std::source_location::current()) {
    if(SUCCEEDED(result))return;
    std::fprintf(stderr,"graphics fixture API failed: line=%u hr=%08lX\n",at.line(),static_cast<unsigned long>(result));
    if(fixtureDevice)std::fprintf(stderr,"device removed reason=%08lX\n",static_cast<unsigned long>(fixtureDevice->GetDeviceRemovedReason()));
    throw std::runtime_error("graphics fixture API failed");
}
void GeometryEvidence(ID3D11Buffer* buffer,ID3D11InputLayout* layout,unsigned frame) {
    taa::fullscreen::Position p;UINT size=sizeof(p);const bool remembered=layout&&SUCCEEDED(layout->GetPrivateData(taa::fullscreen::LayoutKey,&size,&p))&&size==sizeof(p)&&p.valid;
    const auto shadow=taa::constants::Find(buffer);
    if(!shadow){std::printf("TAA geometry evidence: frame=%u layout=%d shadow=false\n",frame,remembered);return;}
    std::lock_guard lock(shadow->mutex);
    std::printf("TAA geometry evidence: frame=%u layout=%d shadow=true generation=%llu domain=%llu serial=%llu known=%zu/%zu\n",frame,remembered,static_cast<unsigned long long>(shadow->generation),static_cast<unsigned long long>(shadow->domain?shadow->domain->revision.load():0),static_cast<unsigned long long>(shadow->writeSerial),static_cast<size_t>(std::count(shadow->known.begin(),shadow->known.end(),static_cast<unsigned char>(1))),shadow->known.size());
}
ComPtr<ID3DBlob> Compile(const char* text,const char* target) {
    ComPtr<ID3DBlob> code,errors;
    const auto result=D3DCompile(text,std::strlen(text),"TAA hook host",nullptr,nullptr,"main",target,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors);
    if(FAILED(result)&&errors)std::fprintf(stderr,"%s",static_cast<const char*>(errors->GetBufferPointer()));
    Hr(result);return code;
}
float Half(unsigned short bits) {
    const unsigned exp=(bits>>10)&31,frac=bits&1023;
    const float value=exp ? std::ldexp(1.0f+float(frac)/1024.0f,int(exp)-15) : std::ldexp(float(frac),-24);
    return bits&0x8000 ? -value : value;
}
}
#include "TaaPackedHookHost.inl"
#include "TaaMatrixPostFixture.h"
#include "BeforeSrFloatFixture.h"
int wmain(int argc,wchar_t** argv) {
    try {
        std::setvbuf(stdout,nullptr,_IONBF,0);
        Check(argc==2||argc==3,"expected fixture case and optional factory route");
        const std::wstring factoryRoute=argc==3?argv[2]:L"default";
        Check(factoryRoute==L"default"||factoryRoute==L"native"||factoryRoute==L"wrapped","known factory route");
        const bool explicitFactory=factoryRoute!=L"default";
        const std::wstring mode=argv[1];
        const auto floatCase=before_sr_float::Find(mode);
        if(mode.starts_with(L"auto-packed"))return RunPackedHook(mode);
        const bool observationJoined=mode==L"auto-observation-joined";
        const bool compute=mode.find(L"compute")!=std::wstring::npos;
        const bool upscale=mode.find(L"upscale")!=std::wstring::npos;
        const bool automatic=mode.starts_with(L"auto");
        const bool projection=mode.starts_with(L"auto-projection");
        const unsigned depthCoverage=mode==L"auto-projection-no-dsv-default"?1:mode==L"auto-projection-no-dsv-enabled"?2:mode==L"auto-projection-dsv-always"?3:mode==L"auto-projection-dsv-less"?4:0;
        const bool projectionPositive=mode==L"auto-projection"||mode==L"auto-projection-timed"||(depthCoverage&&depthCoverage!=4);
        const bool extentRaster=mode==L"auto-raster-extent";
        const bool timedRaster=mode==L"auto-raster-timed"||extentRaster;
        const bool timed=timedRaster||mode==L"auto-projection-timed";
        const bool warmupRaster=mode==L"auto-raster-warmup";
        const bool rasterEvidence=mode==L"auto-raster-evidence"||timedRaster||warmupRaster||observationJoined;
        const bool noJitter=mode==L"auto-no-jitter"||rasterEvidence||projection;
        const bool unsupported=mode==L"auto-unsupported";
        const bool autoSwitch=mode==L"auto-switch";
        const bool linked=mode.find(L"linked")!=std::wstring::npos;
        const bool replacing=projectionPositive||(mode!=L"off"&&mode!=L"discover"&&mode!=L"skip"&&!noJitter&&!unsupported);
        const bool sourceReturn=mode==L"auto-source-return";
        const bool continuity=mode.find(L"continuity")!=std::wstring::npos;
        const bool matrixPost=mode.find(L"matrix")!=std::wstring::npos;
        const bool fullsize=mode.find(L"fullsize")!=std::wstring::npos||sourceReturn;
        const bool postReplay=mode.find(L"replay")!=std::wstring::npos;
        const bool pattern=mode.find(L"pattern")!=std::wstring::npos;
        const bool patternFlip=pattern&&mode.find(L"flip")!=std::wstring::npos;
        const bool opaqueConsumer=mode==L"auto-fullsize-opaque";
        constexpr unsigned render=256;const unsigned output=upscale?512:256;
        const unsigned present=fullsize?427:mode==L"auto-pixel-larger-screen"?output+16:output; // fullsize cases exercise 0.6 source -> true output
        auto pixel=Compile("struct O{float4 color:SV_Target0;float4 history:SV_Target1;};O main(float4 p:SV_Position){O o;o.color=float4(1,0,1,1);o.history=float4(0,1,0,1);return o;}","ps_5_0");
        auto csCode=Compile("RWTexture2D<float4> output:register(u0);RWTexture2D<float4> history:register(u1);[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID){output[p.xy]=float4(1,0,1,1);history[p.xy]=float4(0,1,0,1);}","cs_5_0");
        std::string automaticSource=R"(
Texture2D<float> depth:register(t0);Texture2D<float2> motion:register(t1);
Texture2D<float4> color:register(t2),previous:register(t3);SamplerState linearSampler:register(s0);
cbuffer View:register(b0){float2 inverseSize;float2 jitter;}
float4 temporal(float2 position) {
 float2 uv=position*inverseSize;
 float z=depth.SampleLevel(linearSampler,uv,0),neighbor=depth.SampleLevel(linearSampler,uv+inverseSize,0);
 float2 v=motion.SampleLevel(linearSampler,uv+float2(neighbor<z?inverseSize.x:0,0),0);
 float4 current=color.SampleLevel(linearSampler,uv-jitter,0),old=previous.SampleLevel(linearSampler,uv-v,0);
 return float4(1,0,1,1)+(current+old+min(z,neighbor))*.00001;
}
)";
        if(noJitter)automaticSource.replace(automaticSource.find("uv-jitter"),9,"uv");
        if(unsupported)automaticSource.insert(automaticSource.find("float4 current="),"clip(z-.1);");
        const std::string automaticPixel=automaticSource+"struct O{float4 color:SV_Target0;float4 history:SV_Target1;};O main(float4 p:SV_Position){O o;o.color=temporal(p.xy);o.history=float4(0,1,0,1);return o;}";
        const std::string automaticCompute=automaticSource+"RWTexture2D<float4> output:register(u0);RWTexture2D<float4> history:register(u1);[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID){output[p.xy]=temporal(float2(p.xy)+.5);history[p.xy]=float4(0,1,0,1);}";
        if(automatic) {
            if(compute)csCode=Compile(automaticCompute.c_str(),"cs_5_0");
            else pixel=Compile(automaticPixel.c_str(),"ps_5_0");
        }
        if(observationJoined) {
            std::ifstream file("unresolved-temporal.ps.dxbc",std::ios::binary);const std::vector<char> bytes((std::istreambuf_iterator<char>(file)),{});
            Check(bytes.size()==4668&&DrawObservationDx11::ShaderByteHash(bytes.data(),bytes.size())==0xB12FF59247A26796ull,"complete content-identified B12 observation input");
            pixel.Reset();Hr(D3DCreateBlob(bytes.size(),&pixel));std::memcpy(pixel->GetBufferPointer(),bytes.data(),bytes.size());
        }
        auto plainPixel=Compile("struct O{float4 color:SV_Target0;float4 history:SV_Target1;};O main(float4 p:SV_Position){O o;o.color=float4(1,0,1,1);o.history=float4(0,1,0,1);return o;}","ps_5_0");
        ComPtr<ID3DBlob> alternatePixel;
        if(autoSwitch){auto alternateSource=automaticPixel;alternateSource.replace(alternateSource.find("*.00001"),7,"*.00002");alternatePixel=Compile(alternateSource.c_str(),"ps_5_0");}
        const auto& selected=compute?csCode:pixel;
        const auto hash=DrawObservationDx11::ShaderByteHash(selected->GetBufferPointer(),selected->GetBufferSize());
        std::ofstream profile("taa-profile.ini");
        profile<<"[TAA.Profile]\nSchema=1\nStage="<<(compute?"compute":"pixel")<<"\nShaderHash="<<std::hex<<std::setw(16)<<std::setfill('0')<<hash
            <<"\nColorSlot=2\nDepthSlot=0\nMotionSlot=1\nOutputSlot=0\nMotionEncoding=linear\nMotionUnits=pixels\nMotionScaleX=1\nMotionScaleY=1\nMotionJittered=false\nJitterSlot=0\nJitterOffsetX=0\nJitterOffsetY=4\nJitterUnits=pixels\nJitterScaleX=1\nJitterScaleY=1\nDepthInverted=false\nHDR=false\nHistory=preserve-original\nThreadsX=8\nThreadsY=8\n";
        if(fullsize)profile<<"OutputRoute=observe-upscale\n";
        profile.close();
        const auto module=LoadLibraryW(L"OptiScaler.dll");Check(module!=nullptr,"load actual isolated F6 DLL");
        // Let startup install factory routes. Pixel delivery is checked below;
        // elapsed time is never used as evidence of GPU retirement.
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        WNDCLASSW windowClass{};windowClass.lpfnWndProc=DefWindowProcW;windowClass.lpszClassName=L"F6TaaHookFixture";windowClass.hInstance=GetModuleHandleW(nullptr);
        Check(RegisterClassW(&windowClass)!=0,"register fixture window");
        const auto window=CreateWindowExW(0,windowClass.lpszClassName,L"F6 TAA fixture",WS_OVERLAPPEDWINDOW,0,0,512,512,nullptr,nullptr,windowClass.hInstance,nullptr);
        Check(window!=nullptr,"create hidden fixture window");
        DXGI_SWAP_CHAIN_DESC chainDesc{};chainDesc.BufferDesc.Width=chainDesc.BufferDesc.Height=present;
        chainDesc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;chainDesc.BufferCount=2;chainDesc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
        chainDesc.OutputWindow=window;chainDesc.SampleDesc.Count=1;chainDesc.Windowed=TRUE;chainDesc.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;ComPtr<IDXGISwapChain> chain;
        if(sourceReturn||explicitFactory) {
            using CreateFactory=HRESULT(WINAPI*)(REFIID,void**);
            const auto create=reinterpret_cast<CreateFactory>(GetProcAddress(module,"CreateDXGIFactory1"));
            Check(create!=nullptr,"actual F6 public DXGI factory export");
            ComPtr<IDXGIFactory2> factory;const auto deadline=GetTickCount64()+10000;
            for(;;) {
                factory.Reset();Hr(create(IID_PPV_ARGS(&factory)));
                std::ifstream stream("OptiScaler.log");const std::string text((std::istreambuf_iterator<char>(stream)),{});
                if(text.find("hkCreateDXGIFactory1 Caller:")!=std::string::npos)break;
                Check(GetTickCount64()<deadline,"actual F6 factory hooks installed");Sleep(20);
            }
            if(explicitFactory) {
                constexpr GUID wrappedFactoryId{0x1af622a3,0x82d0,0x49cd,{0x12,0x4f,0xcc,0xe0,0x51,0x22,0xc2,0x22}};
                ComPtr<IUnknown> wrapper;const auto query=factory->QueryInterface(wrappedFactoryId,&wrapper);
                Check(factoryRoute==L"wrapped"?SUCCEEDED(query):query==E_NOINTERFACE,"actual public factory route identity");
                std::printf("PASS actual public factory route: %ls wrapped=%s hwnd=%p\n",factoryRoute.c_str(),wrapper?"true":"false",window);
            }
            Hr(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,matrixPost?D3D11_CREATE_DEVICE_DEBUG:0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
            DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=desc.Height=present;desc.Format=chainDesc.BufferDesc.Format;
            desc.BufferCount=desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT|DXGI_USAGE_SHADER_INPUT;
            desc.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;ComPtr<IDXGISwapChain1> interop;
            Hr(factory->CreateSwapChainForHwnd(device.Get(),window,&desc,nullptr,nullptr,&interop));Hr(interop.As(&chain));
        } else Hr(D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,matrixPost?D3D11_CREATE_DEVICE_DEBUG:0,nullptr,0,D3D11_SDK_VERSION,&chainDesc,&chain,&device,nullptr,&context));
        fixtureDevice=device.Get();
        const auto texture=[&](unsigned size,DXGI_FORMAT format,UINT extraBindings=D3D11_BIND_UNORDERED_ACCESS) {
            D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=size;d.Format=format;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
            d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET|extraBindings;
            ComPtr<ID3D11Texture2D> t;Hr(device->CreateTexture2D(&d,nullptr,&t));return t;
        };
        std::array<ComPtr<ID3D11Texture2D>,3> inputs{texture(render,DXGI_FORMAT_R32_FLOAT),texture(render,DXGI_FORMAT_R16G16_FLOAT),texture(render,floatCase?floatCase->format:DXGI_FORMAT_R16G16B16A16_FLOAT,floatCase?0:D3D11_BIND_UNORDERED_ACCESS)};
        ComPtr<ID3D11DepthStencilView> automaticDepth;
        if(automatic){D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=render;d.Format=DXGI_FORMAT_R32_TYPELESS;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.BindFlags=D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE;inputs[0].Reset();Hr(device->CreateTexture2D(&d,nullptr,&inputs[0]));D3D11_DEPTH_STENCIL_VIEW_DESC dv{};dv.Format=DXGI_FORMAT_D32_FLOAT;dv.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;Hr(device->CreateDepthStencilView(inputs[0].Get(),&dv,&automaticDepth));}
        std::array<ComPtr<ID3D11ShaderResourceView>,3> views;
        for(unsigned i=0;i<3;++i) {
            if(automatic&&i==0){D3D11_SHADER_RESOURCE_VIEW_DESC v{};v.Format=DXGI_FORMAT_R32_FLOAT;v.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;v.Texture2D.MipLevels=1;Hr(device->CreateShaderResourceView(inputs[0].Get(),&v,&views[0]));continue;}
            ComPtr<ID3D11RenderTargetView> rtv;Hr(device->CreateRenderTargetView(inputs[i].Get(),nullptr,&rtv));
            const float value=i==0?.5f:i==1?0.0f:.25f;const float color[4]{value,value,value,1};context->ClearRenderTargetView(rtv.Get(),color);
            Hr(device->CreateShaderResourceView(inputs[i].Get(),nullptr,&views[i]));
        }
        if(pattern) {
            std::vector<unsigned short> pixels(size_t(render)*render*4,0x3400);
            for(unsigned y=0;y<render;++y)for(unsigned x=0;x<render;++x) {
                const unsigned short value=y<render/4?0x3000:y>=render*3/4?0x3800:x<render/4?0x3600:x>=render*3/4?0x3900:0x3400;
                const auto at=(size_t(y)*render+x)*4;for(unsigned c=0;c<3;++c)pixels[at+c]=value;pixels[at+3]=0x3c00;
            }
            context->UpdateSubresource(inputs[2].Get(),0,nullptr,pixels.data(),render*8,0);
        }
        auto previousColor=texture(output,DXGI_FORMAT_R16G16B16A16_FLOAT);ComPtr<ID3D11ShaderResourceView> previousView;Hr(device->CreateShaderResourceView(previousColor.Get(),nullptr,&previousView));
        ComPtr<ID3D11RenderTargetView> previousRtv;Hr(device->CreateRenderTargetView(previousColor.Get(),nullptr,&previousRtv));const float previousValue[]{.25f,.25f,.25f,1};context->ClearRenderTargetView(previousRtv.Get(),previousValue);
        ComPtr<ID3D11SamplerState> temporalSampler;D3D11_SAMPLER_DESC sampler{};sampler.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sampler.MaxLOD=D3D11_FLOAT32_MAX;Hr(device->CreateSamplerState(&sampler,&temporalSampler));
        auto target=texture(output,DXGI_FORMAT_R16G16B16A16_FLOAT),history=texture(output,DXGI_FORMAT_R16G16B16A16_FLOAT);
        ComPtr<ID3D11RenderTargetView> targetRtv,historyRtv;ComPtr<ID3D11UnorderedAccessView> targetUav,historyUav;
        Hr(device->CreateRenderTargetView(target.Get(),nullptr,&targetRtv));Hr(device->CreateRenderTargetView(history.Get(),nullptr,&historyRtv));
        Hr(device->CreateUnorderedAccessView(target.Get(),nullptr,&targetUav));Hr(device->CreateUnorderedAccessView(history.Get(),nullptr,&historyUav));
        std::array<ComPtr<ID3D11Texture2D>,2> separateTextures;std::array<ComPtr<ID3D11UnorderedAccessView>,2> separateUavs;
        if(mode==L"auto-fullsize-dispatch")for(unsigned i=0;i<2;++i){separateTextures[i]=texture(render,DXGI_FORMAT_R16G16B16A16_FLOAT);Hr(device->CreateUnorderedAccessView(separateTextures[i].Get(),nullptr,&separateUavs[i]));}
        D3D11_BUFFER_DESC cbDesc{};cbDesc.ByteWidth=observationJoined?168*16:16;cbDesc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;cbDesc.Usage=D3D11_USAGE_DYNAMIC;cbDesc.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
        ComPtr<ID3D11Buffer> cb;Hr(device->CreateBuffer(&cbDesc,nullptr,&cb));
        ComPtr<ID3D11DeviceContext1> context1;ComPtr<ID3DDeviceContextState> alternate;
        if(mode==L"state-switch") {
            ComPtr<ID3D11Device1> device1;Hr(device.As(&device1));Hr(context.As(&context1));
            const auto level=device->GetFeatureLevel();D3D_FEATURE_LEVEL chosen{};
            Hr(device1->CreateDeviceContextState(0,&level,1,D3D11_SDK_VERSION,__uuidof(ID3D11Device1),&chosen,&alternate));
        }
        ComPtr<ID3D11DepthStencilState> resolveDepthState;ComPtr<ID3D11DepthStencilView> resolveDepthView;
        if(depthCoverage) {
            D3D11_DEPTH_STENCIL_DESC d{};d.DepthEnable=TRUE;d.DepthWriteMask=depthCoverage==4?D3D11_DEPTH_WRITE_MASK_ZERO:D3D11_DEPTH_WRITE_MASK_ALL;
            d.DepthFunc=depthCoverage==3?D3D11_COMPARISON_ALWAYS:D3D11_COMPARISON_LESS;
            Hr(device->CreateDepthStencilState(&d,&resolveDepthState));
            if(depthCoverage>=3) {
                D3D11_TEXTURE2D_DESC t{};t.Width=t.Height=output;t.Format=DXGI_FORMAT_D32_FLOAT;t.MipLevels=t.ArraySize=t.SampleDesc.Count=1;t.BindFlags=D3D11_BIND_DEPTH_STENCIL;
                std::vector<float> initial(size_t(output)*output,1);D3D11_SUBRESOURCE_DATA data{initial.data(),output*unsigned(sizeof(float)),0};
                ComPtr<ID3D11Texture2D> depth;Hr(device->CreateTexture2D(&t,&data,&depth));
                D3D11_DEPTH_STENCIL_VIEW_DESC v{};v.Format=t.Format;v.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;v.Flags=depthCoverage==4?D3D11_DSV_READ_ONLY_DEPTH:0;
                Hr(device->CreateDepthStencilView(depth.Get(),&v,&resolveDepthView));
            }
        }
        auto vsCode=Compile("float4 main(uint id:SV_VertexID):SV_Position{return float4(float2((id<<1)&2,id&2)*float2(2,-2)+float2(-1,1),0,1);}","vs_5_0");
        ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;ComPtr<ID3D11ComputeShader> cs;
        ComPtr<ID3D11PixelShader> alternatePs,plainPs;
        ComPtr<ID3D11RenderTargetView> presentRtv,postRtv;ComPtr<ID3D11ShaderResourceView> presentInput,postInput;ComPtr<ID3D11PixelShader> presentPs,postPs;
        ComPtr<ID3D11Texture2D> presentTexture,postTexture;ComPtr<ID3D11VertexShader> presentVs,postVs;ComPtr<ID3D11InputLayout> presentLayout;ComPtr<ID3D11Buffer> presentVertices;
        if(fullsize) {
            const auto load=[](const char* path){std::ifstream in(path,std::ios::binary);const std::vector<char> bytes((std::istreambuf_iterator<char>(in)),{});Check(!bytes.empty(),"exact captured consumer bytecode");ComPtr<ID3DBlob> code;Hr(D3DCreateBlob(bytes.size(),&code));std::memcpy(code->GetBufferPointer(),bytes.data(),bytes.size());return code;};
            auto p=load("consumer.ps.dxbc"),v=load("consumer.vs.dxbc");
            Check(DrawObservationDx11::ShaderByteHash(p->GetBufferPointer(),p->GetBufferSize())==0xF63F3FF630FEBDB4ull&&DrawObservationDx11::ShaderByteHash(v->GetBufferPointer(),v->GetBufferSize())==0x55E141E63591B33Cull,"actual game consumer program identity");
            Hr(device->CreatePixelShader(p->GetBufferPointer(),p->GetBufferSize(),nullptr,&presentPs));
            if(pattern) {
                const std::string code="struct O{float2 uv:TEXCOORD;float4 p:SV_Position;};O main(float2 p:POSITION){O o;o.p=float4(p*2,0,1);o.uv=p*float2(1,"+std::string(patternFlip?"1":"-1")+")+.5;return o;}";
                v=Compile(code.c_str(),"vs_5_0");
            }
            if(!pattern) {
                // PSF63 consumes TEXCOORD in register 0; the unrelated captured
                // VS55 writes it in register 1. Keep a physically valid pair.
                v=Compile("struct O{float2 uv:TEXCOORD;float4 p:SV_Position;};O main(float2 p:POSITION){O o;o.p=float4(p,0,1);o.uv=p*float2(.5,-.5)+.5;return o;}","vs_5_0");
            }
            if(pattern&&postReplay) {auto code=load("post.ps.dxbc");Check(DrawObservationDx11::ShaderByteHash(code->GetBufferPointer(),code->GetBufferSize())==0x88CBD14623C28B8Eull,"real independent postprocess bytecode");Hr(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&postPs));
                // Captured PS88 reads TEXCOORD at v1; the final PSF63 reads v0.
                auto pv=Compile("struct O{float4 p:SV_Position;float2 uv:TEXCOORD;};O main(float2 p:POSITION){O o;o.p=float4(p*2,0,1);o.uv=p*float2(1,-1)+.5;return o;}","vs_5_0");
                Hr(device->CreateVertexShader(pv->GetBufferPointer(),pv->GetBufferSize(),nullptr,&postVs));}
            Hr(device->CreateVertexShader(v->GetBufferPointer(),v->GetBufferSize(),nullptr,&presentVs));
            const D3D11_INPUT_ELEMENT_DESC element{"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0};Hr(device->CreateInputLayout(&element,1,v->GetBufferPointer(),v->GetBufferSize(),&presentLayout));
            const float scale=pattern?.5f:1.0f;const float vertices[]{-scale,scale,3*scale,scale,-scale,-3*scale};D3D11_BUFFER_DESC d{};d.ByteWidth=sizeof(vertices);d.BindFlags=D3D11_BIND_VERTEX_BUFFER;D3D11_SUBRESOURCE_DATA init{vertices,0,0};Hr(device->CreateBuffer(&d,&init,&presentVertices));
            GeometryEvidence(presentVertices.Get(),presentLayout.Get(),999);
            // DEFAULT static VB is created before first Present and is never
            // rewritten. Only the actual creation hooks can prove its content.
            Hr(chain->GetBuffer(0,IID_PPV_ARGS(&presentTexture)));Hr(device->CreateRenderTargetView(presentTexture.Get(),nullptr,&presentRtv));
            Hr(device->CreateShaderResourceView(target.Get(),nullptr,&presentInput));
            if(postReplay){postTexture=texture(output,DXGI_FORMAT_R16G16B16A16_FLOAT);Hr(device->CreateRenderTargetView(postTexture.Get(),nullptr,&postRtv));Hr(device->CreateShaderResourceView(postTexture.Get(),nullptr,&postInput));}
        }
        TaaMatrixPostFixture matrixPostFixture;
        if(matrixPost)matrixPostFixture.Init(device.Get(),present,chainDesc.BufferDesc.Format);
        ComPtr<ID3D11ClassLinkage> linkage;
        if(linked)Hr(device->CreateClassLinkage(&linkage));
        Hr(device->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vs));
        Hr(device->CreatePixelShader(pixel->GetBufferPointer(),pixel->GetBufferSize(),linkage.Get(),&ps));
        if(sourceReturn||continuity)Hr(device->CreatePixelShader(plainPixel->GetBufferPointer(),plainPixel->GetBufferSize(),nullptr,&plainPs));
        Hr(device->CreateComputeShader(csCode->GetBufferPointer(),csCode->GetBufferSize(),linkage.Get(),&cs));
        ComPtr<ID3D11VertexShader> sceneVs;ComPtr<ID3D11PixelShader> scenePs;
        ComPtr<ID3D11Buffer> sceneCb,sceneIndices;ComPtr<ID3D11RenderTargetView> sceneRtv,sceneMotionRtv;
        ComPtr<ID3DBlob> sceneVsCode;
        if(rasterEvidence) {
            sceneVsCode=Compile("cbuffer Raster:register(b2){float4 shift;}float4 main(uint id:SV_VertexID):SV_Position{return float4(float2((id<<1)&2,id&2)*float2(2,-2)+float2(-1,1)+shift.xy,0,1);}","vs_5_0");
            auto scenePsCode=Compile(observationJoined?"struct O{float4 c:SV_Target0;float2 m:SV_Target1;};O main(){O o;o.c=.25;o.m=.498039;return o;}":"float4 main():SV_Target0{return float4(.25,.25,.25,1);}","ps_5_0");
            Hr(device->CreateVertexShader(sceneVsCode->GetBufferPointer(),sceneVsCode->GetBufferSize(),nullptr,&sceneVs));
            Hr(device->CreatePixelShader(scenePsCode->GetBufferPointer(),scenePsCode->GetBufferSize(),nullptr,&scenePs));
            Hr(device->CreateBuffer(&cbDesc,nullptr,&sceneCb));Hr(device->CreateRenderTargetView(inputs[2].Get(),nullptr,&sceneRtv));
            if(observationJoined)Hr(device->CreateRenderTargetView(inputs[1].Get(),nullptr,&sceneMotionRtv));
            const unsigned indices[]{0,1,2,0,1,2};D3D11_BUFFER_DESC ib{};ib.ByteWidth=sizeof(indices);ib.Usage=D3D11_USAGE_IMMUTABLE;ib.BindFlags=D3D11_BIND_INDEX_BUFFER;
            D3D11_SUBRESOURCE_DATA data{indices,0,0};Hr(device->CreateBuffer(&ib,&data,&sceneIndices));
        }
        if(alternatePixel)Hr(device->CreatePixelShader(alternatePixel->GetBufferPointer(),alternatePixel->GetBufferSize(),nullptr,&alternatePs));
        D3D11_DEPTH_STENCIL_DESC ds{};ds.DepthFunc=D3D11_COMPARISON_ALWAYS;ComPtr<ID3D11DepthStencilState> noDepth;Hr(device->CreateDepthStencilState(&ds,&noDepth));
        // Use the exact captured motion programs, not a fixture-only product
        // fingerprint or API. The synthetic camera has a known projection
        // offset while the resolve shader deliberately has no jitter read.
        ComPtr<ID3D11PixelShader> projectionPs;ComPtr<ID3D11VertexShader> projectionVs;
        ComPtr<ID3D11InputLayout> projectionLayout;ComPtr<ID3D11Buffer> projectionCb,projectionRays,projectionDepth,projectionVertices,projectionIndices,projectionInitial;
        ComPtr<ID3D11Texture2D> otherMotion,motionDepth;ComPtr<ID3D11ShaderResourceView> motionDepthView;
        ComPtr<ID3D11RenderTargetView> projectionRt;
        if(projection) {
            const auto load=[](const char* name){std::ifstream f(name,std::ios::binary);std::vector<char> data{std::istreambuf_iterator<char>(f),{}};Check(!data.empty(),"exact captured projection DXBC input");return data;};
            const auto p=load("projection-motion.ps.dxbc"),v=load("projection-motion.vs.dxbc");
            Check(DrawObservationDx11::ShaderByteHash(p.data(),p.size())==0x137B32A04AB724F0ull&&DrawObservationDx11::ShaderByteHash(v.data(),v.size())==0xF9C1F953EE80980Aull,"verified captured motion PS/VS identities");
            Hr(device->CreatePixelShader(p.data(),p.size(),nullptr,&projectionPs));Hr(device->CreateVertexShader(v.data(),v.size(),nullptr,&projectionVs));
            const D3D11_INPUT_ELEMENT_DESC element{"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0};
            Hr(device->CreateInputLayout(&element,1,v.data(),v.size(),&projectionLayout));
            const auto immutable=[&](const void* data,UINT size,UINT flags,ComPtr<ID3D11Buffer>& result){D3D11_BUFFER_DESC d{};d.ByteWidth=size;d.BindFlags=flags;d.Usage=D3D11_USAGE_IMMUTABLE;D3D11_SUBRESOURCE_DATA initial{data,0,0};Hr(device->CreateBuffer(&d,&initial,&result));};
            const float vertices[]{-1,-1,-1,3,3,-1};const unsigned indices[]{0,1,2};
            immutable(vertices,sizeof(vertices),D3D11_BIND_VERTEX_BUFFER,projectionVertices);immutable(indices,sizeof(indices),D3D11_BIND_INDEX_BUFFER,projectionIndices);
            std::array<float,40> rays{};const float directions[]{-.5f,.3125f,-1,0,-.5f,-.9375f,-1,0,1.5f,.3125f,-1,0};std::memcpy(rays.data()+28,directions,sizeof(directions));
            immutable(rays.data(),sizeof(rays),D3D11_BIND_CONSTANT_BUFFER,projectionRays);
            const float linearDepth[8]{0,0,0,0,0,1,0,0};immutable(linearDepth,sizeof(linearDepth),D3D11_BIND_CONSTANT_BUFFER,projectionDepth);
            const auto matrix=taa::test::ProjectionConstants(render,render);immutable(matrix.data(),sizeof(matrix),D3D11_BIND_CONSTANT_BUFFER,projectionInitial);
            D3D11_BUFFER_DESC d{};d.ByteWidth=sizeof(matrix);d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;d.Usage=D3D11_USAGE_DYNAMIC;d.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;Hr(device->CreateBuffer(&d,nullptr,&projectionCb));
            otherMotion=texture(render,DXGI_FORMAT_R16G16_FLOAT);auto* destination=mode==L"auto-projection-mismatch"?otherMotion.Get():inputs[1].Get();Hr(device->CreateRenderTargetView(destination,nullptr,&projectionRt));
            motionDepth=texture(render,DXGI_FORMAT_R32_FLOAT);Hr(device->CreateShaderResourceView(motionDepth.Get(),nullptr,&motionDepthView));
        }
        const auto drawProjection=[&](unsigned frame) {
            const auto matrix=taa::test::ProjectionConstants(render,render,frame%2?.25f:-.25f,frame%2?-.125f:.125f);
            if(mode==L"auto-projection-unknown")context->CopyResource(projectionCb.Get(),projectionInitial.Get());
            else {D3D11_MAPPED_SUBRESOURCE mapped{};Hr(context->Map(projectionCb.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped));std::memcpy(mapped.pData,matrix.data(),sizeof(matrix));context->Unmap(projectionCb.Get(),0);}
            ID3D11Buffer* vertex=projectionVertices.Get();UINT stride=8,offset=0;context->IASetInputLayout(projectionLayout.Get());context->IASetVertexBuffers(0,1,&vertex,&stride,&offset);context->IASetIndexBuffer(projectionIndices.Get(),DXGI_FORMAT_R32_UINT,0);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ID3D11Buffer* psBuffers[]{projectionCb.Get(),projectionDepth.Get()};ID3D11Buffer* vsBuffer=projectionRays.Get();context->PSSetConstantBuffers(0,2,psBuffers);context->VSSetConstantBuffers(0,1,&vsBuffer);
            context->VSSetShader(projectionVs.Get(),nullptr,0);context->PSSetShader(projectionPs.Get(),nullptr,0);
            ID3D11ShaderResourceView* depthInput=motionDepthView.Get();ID3D11SamplerState* samplerState=temporalSampler.Get();context->PSSetShaderResources(0,1,&depthInput);context->PSSetSamplers(0,1,&samplerState);
            ID3D11RenderTargetView* rt=projectionRt.Get();context->OMSetRenderTargets(1,&rt,automaticDepth.Get());context->OMSetDepthStencilState(noDepth.Get(),0);D3D11_VIEWPORT viewport{0,0,float(render),float(render),0,1};context->RSSetViewports(1,&viewport);
            context->DrawIndexed(3,0,0);context->ClearState();
        };
        const auto read=[&](ID3D11Texture2D* source,float fx=.5f,float fy=.5f) {
            D3D11_TEXTURE2D_DESC d{};source->GetDesc(&d);d.Usage=D3D11_USAGE_STAGING;d.BindFlags=d.MiscFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> staging;Hr(device->CreateTexture2D(&d,nullptr,&staging));context->CopyResource(staging.Get(),source);
            D3D11_MAPPED_SUBRESOURCE mapped{};Hr(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));
            const auto* pixelBytes=static_cast<const unsigned char*>(mapped.pData)+unsigned(d.Height*fy)*mapped.RowPitch+unsigned(d.Width*fx)*before_sr_float::Stride(d.Format);
            const auto values=before_sr_float::Decode(pixelBytes,d.Format);
            context->Unmap(staging.Get(),0);return values;
        };
        unsigned checked=0,gray=0,magenta=0,greenHistory=0,rejectedJitter=0;
        const unsigned frames=continuity?72:sourceReturn?72:extentRaster?145:timed?97:depthCoverage==4?326:24;
        unsigned sourceBefore=0,sourceAfter=0,deliveredGray=0,recoveryLearning=0,directionalFrames=0,continuityGap=0,matrixVerified=0,matrixMismatches=0;
        std::array<float,4> lastDirections{};
        const auto captureStart=std::chrono::steady_clock::now();
        unsigned floatPhase=0,floatConsecutive=0,floatVerified=0;
        const auto floatDeadline=GetTickCount64()+25000;
        for(unsigned frame=0;floatCase?(floatPhase<3&&GetTickCount64()<floatDeadline):frame<frames;++frame) {
            const bool absent=continuity&&frame>=24&&frame<32;
            const bool jitterGap=continuity&&frame==44;
            const bool consumerGap=continuity&&fullsize&&frame==56;
            std::optional<before_sr_float::Pixels> floatSource;
            if(floatCase) {
                context->ClearState();
                ComPtr<ID3D11RenderTargetView> sourceRtv;Hr(device->CreateRenderTargetView(inputs[2].Get(),nullptr,&sourceRtv));
                const auto color=before_sr_float::Source(floatPhase);context->ClearRenderTargetView(sourceRtv.Get(),color.data());
                floatSource=before_sr_float::Read(device.Get(),context.Get(),inputs[2].Get());
            }
            if(timed&&frame&&frame%24==0) {
                constexpr unsigned offsets[]{0,5000,15000,30000,35500,38500,41000};
                std::this_thread::sleep_until(captureStart+std::chrono::milliseconds(offsets[frame/24]+200));
            }
            if(extentRaster&&(frame==96||frame==120)) {
                // A real render-extent change replaces the entire scene input set.
                // Keep mismatched-guide rejection in the product; do not fake it
                // by resizing only the colour resource in this fixture.
                context->ClearState();sceneRtv.Reset();automaticDepth.Reset();
                for(auto& view:views)view.Reset();
                const unsigned sceneSize=frame==96?154:render;
                D3D11_TEXTURE2D_DESC depthDesc{};depthDesc.Width=depthDesc.Height=sceneSize;
                depthDesc.Format=DXGI_FORMAT_R32_TYPELESS;depthDesc.MipLevels=depthDesc.ArraySize=depthDesc.SampleDesc.Count=1;
                depthDesc.BindFlags=D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE;
                inputs[0].Reset();Hr(device->CreateTexture2D(&depthDesc,nullptr,&inputs[0]));
                D3D11_DEPTH_STENCIL_VIEW_DESC depthView{};depthView.Format=DXGI_FORMAT_D32_FLOAT;depthView.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
                Hr(device->CreateDepthStencilView(inputs[0].Get(),&depthView,&automaticDepth));
                D3D11_SHADER_RESOURCE_VIEW_DESC depthSrv{};depthSrv.Format=DXGI_FORMAT_R32_FLOAT;depthSrv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;depthSrv.Texture2D.MipLevels=1;
                Hr(device->CreateShaderResourceView(inputs[0].Get(),&depthSrv,&views[0]));
                inputs[1]=texture(sceneSize,DXGI_FORMAT_R16G16_FLOAT);
                inputs[2]=texture(sceneSize,DXGI_FORMAT_R16G16B16A16_FLOAT);
                for(unsigned i=1;i<3;++i)Hr(device->CreateShaderResourceView(inputs[i].Get(),nullptr,&views[i]));
                Hr(device->CreateRenderTargetView(inputs[2].Get(),nullptr,&sceneRtv));
            }
            if(observationJoined&&frame==4) {
                context->ClearState();sceneRtv.Reset();sceneMotionRtv.Reset();views[1].Reset();views[2].Reset();
                inputs[1]=texture(render,DXGI_FORMAT_R16G16_FLOAT);inputs[2]=texture(render,DXGI_FORMAT_R16G16B16A16_FLOAT);
                Hr(device->CreateShaderResourceView(inputs[1].Get(),nullptr,&views[1]));Hr(device->CreateShaderResourceView(inputs[2].Get(),nullptr,&views[2]));
                Hr(device->CreateRenderTargetView(inputs[2].Get(),nullptr,&sceneRtv));Hr(device->CreateRenderTargetView(inputs[1].Get(),nullptr,&sceneMotionRtv));
            }
            if(mode==L"shutdown"&&frame==12) {
                using Shutdown=unsigned long(__cdecl*)(ID3D11Device*);
                const auto shutdown=reinterpret_cast<Shutdown>(GetProcAddress(module,"NVSDK_NGX_D3D11_Shutdown1"));
                Check(shutdown&&shutdown(device.Get())==1,"ordinary NGX Shutdown1 must retire the actual provider");
            }
            context->ClearState();
            if(rasterEvidence&&(!warmupRaster||frame>=12)) {
                if(!observationJoined||frame>=12) {
                    D3D11_MAPPED_SUBRESOURCE sceneMapped{};Hr(context->Map(sceneCb.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&sceneMapped));
                    const float shift[]{float(frame+1)*.00001f,-float(frame+1)*.00002f,float(frame+1),1234.5f};std::memcpy(sceneMapped.pData,shift,sizeof(shift));context->Unmap(sceneCb.Get(),0);
                }
                ID3D11Buffer* sceneBuffer=sceneCb.Get();context->VSSetConstantBuffers(2,1,&sceneBuffer);
                context->VSSetShader(sceneVs.Get(),nullptr,0);context->PSSetShader(scenePs.Get(),nullptr,0);
                context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->IASetIndexBuffer(sceneIndices.Get(),DXGI_FORMAT_R32_UINT,0);
                D3D11_VIEWPORT viewport{0,0,float(render),float(render),0,1};context->RSSetViewports(1,&viewport);context->OMSetDepthStencilState(noDepth.Get(),0);
                ID3D11RenderTargetView* sceneTargets[]{sceneRtv.Get(),sceneMotionRtv.Get()};context->OMSetRenderTargets(observationJoined?2:1,sceneTargets,nullptr);context->DrawIndexedInstanced(6,2,0,0,0);
                context->ClearState();
            }
            ComPtr<ID3DDeviceContextState> previous;
            if(alternate)context1->SwapDeviceContextState(alternate.Get(),&previous);
            D3D11_MAPPED_SUBRESOURCE mapped{};Hr(context->Map(cb.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped));
            std::memset(mapped.pData,0,cbDesc.ByteWidth);
            if(automatic){const float settings[]{1.0f/float(output),1.0f/float(output),.25f/float(render),-.125f/float(render)};std::memcpy(mapped.pData,settings,sizeof(settings));}
            if(observationJoined) {
                auto* values=static_cast<float*>(mapped.pData);
                values[138*4]=values[138*4+1]=float(render);values[138*4+2]=values[138*4+3]=1.0f/float(render);
                values[139*4]=.6f;values[139*4+2]=12;values[139*4+3]=1;values[167*4+1]=.5f;
            }
            if((alternate&&frame==8)||jitterGap) {
                const float invalid=std::nanf("");std::memcpy(static_cast<unsigned char*>(mapped.pData)+(jitterGap?8:0),&invalid,sizeof(invalid));
            }
            context->Unmap(cb.Get(),0);
            if(previous)context1->SwapDeviceContextState(previous.Get(),nullptr);
            if(automatic)context->ClearDepthStencilView(automaticDepth.Get(),D3D11_CLEAR_DEPTH,1,0);
            if(projection) {
                drawProjection(frame);
                if(mode==L"auto-projection-multi")drawProjection(frame);
                if(mode==L"auto-projection-stale")Hr(chain->Present(0,0));
            }
            ID3D11ShaderResourceView* srvs[]{views[0].Get(),views[1].Get(),views[2].Get(),previousView.Get(),views[2].Get()};ID3D11Buffer* buffer=cb.Get();
            ID3D11SamplerState* sample=temporalSampler.Get();
            if(compute) {
                context->CSSetShader(cs.Get(),nullptr,0);context->CSSetShaderResources(0,automatic?4:3,srvs);context->CSSetConstantBuffers(0,1,&buffer);context->CSSetSamplers(0,1,&sample);
                ID3D11UnorderedAccessView* uavs[]{targetUav.Get(),historyUav.Get()};context->CSSetUnorderedAccessViews(0,2,uavs,nullptr);
                context->Dispatch(output/8,output/8,1);
            } else {
                context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(absent||(sourceReturn&&frame>=24&&frame<48)?plainPs.Get():autoSwitch&&frame>=12?alternatePs.Get():ps.Get(),nullptr,0);
                context->PSSetShaderResources(0,observationJoined?5:automatic?4:3,srvs);context->PSSetConstantBuffers(0,1,&buffer);context->PSSetSamplers(0,1,&sample);context->OMSetDepthStencilState(noDepth.Get(),0);
                D3D11_VIEWPORT vp{0,0,float(output),float(output),0,1};context->RSSetViewports(1,&vp);
                if(depthCoverage)context->OMSetDepthStencilState(depthCoverage==1?nullptr:resolveDepthState.Get(),0);
                if(resolveDepthView&&depthCoverage!=4)context->ClearDepthStencilView(resolveDepthView.Get(),D3D11_CLEAR_DEPTH,1,0);
                ID3D11RenderTargetView* rtvs[]{targetRtv.Get(),historyRtv.Get()};context->OMSetRenderTargets(2,rtvs,resolveDepthView.Get());context->Draw(3,0);
            }
            if(mode==L"auto-fullsize-dispatch") {
                context->ClearState();context->CSSetShader(cs.Get(),nullptr,0);ID3D11UnorderedAccessView* separate[]{separateUavs[0].Get(),separateUavs[1].Get()};
                context->CSSetUnorderedAccessViews(0,2,separate,nullptr);context->Dispatch(render/8,render/8,1);context->ClearState();
            }
            if(opaqueConsumer){ComPtr<ID3D11DeviceContext> deferred;Hr(device->CreateDeferredContext(0,&deferred));ComPtr<ID3D11CommandList> list;Hr(deferred->FinishCommandList(FALSE,&list));context->ExecuteCommandList(list.Get(),TRUE);}
            std::array<float,4> delivered{};
            if(fullsize) {
                const auto consumer=[&](ID3D11ShaderResourceView* input,ID3D11RenderTargetView* targetView,unsigned size,ID3D11PixelShader* consumerPs,ID3D11VertexShader* consumerVs){
                    context->ClearState();context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->IASetInputLayout(presentLayout.Get());
                    ID3D11Buffer* vertex=presentVertices.Get();UINT stride=8,offset=0;context->IASetVertexBuffers(0,1,&vertex,&stride,&offset);
                    context->VSSetShader(consumerVs,nullptr,0);context->PSSetShader(consumerPs,nullptr,0);context->PSSetShaderResources(0,1,&input);context->PSSetSamplers(0,1,&sample);
                    if(matrixPost&&consumerPs==matrixPostFixture.ps.Get())matrixPostFixture.Bind(context.Get());
                    context->OMSetRenderTargets(1,&targetView,nullptr);context->OMSetDepthStencilState(noDepth.Get(),0);D3D11_VIEWPORT viewport{0,0,float(size),float(size),0,1};context->RSSetViewports(1,&viewport);context->Draw(3,0);
                };
                if(matrixPost)matrixPostFixture.Update(context.Get(),frame);
                if(postReplay)consumer(presentInput.Get(),postRtv.Get(),output,matrixPost?matrixPostFixture.ps.Get():postPs?postPs.Get():presentPs.Get(),postVs?postVs.Get():presentVs.Get());
                consumer(postReplay?postInput.Get():presentInput.Get(),presentRtv.Get(),present,consumerGap?plainPs.Get():presentPs.Get(),presentVs.Get());
                delivered=read(presentTexture.Get());
                if(matrixPost&&frame>=6) {
                    consumer(views[2].Get(),matrixPostFixture.oracleRtv.Get(),present,matrixPostFixture.ps.Get(),matrixPostFixture.vs.Get());
                    for(const auto point:std::array<std::array<float,2>,5>{{{.5f,.5f},{.1f,.1f},{.9f,.9f},{.1f,.9f},{.9f,.1f}}}) {
                        const auto expected=read(matrixPostFixture.oracle.Get(),point[0],point[1]);const auto actual=read(presentTexture.Get(),point[0],point[1]);
                        for(unsigned c=0;c<3;++c)if(std::abs(expected[c]-actual[c])>.018f){std::fprintf(stderr,"matrix oracle mismatch frame=%u channel=%u expected=%f actual=%f\n",frame,c,expected[c],actual[c]);++matrixMismatches;}
                    }
                    if(frame==6) {
                        ComPtr<ID3D11InfoQueue> info;if(SUCCEEDED(device.As(&info)))for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T bytes=0;info->GetMessage(i,nullptr,&bytes);std::vector<unsigned char> storage(bytes);auto* message=reinterpret_cast<D3D11_MESSAGE*>(storage.data());if(SUCCEEDED(info->GetMessage(i,message,&bytes))&&message->Severity<=D3D11_MESSAGE_SEVERITY_WARNING)std::fprintf(stderr,"D3D matrix replay: %s\n",message->pDescription);}
                    }
                    ++matrixVerified;
                }
            }
            Hr(chain->Present(0,0));
            if(fullsize&&(frame==0||frame==1||frame==6))GeometryEvidence(presentVertices.Get(),presentLayout.Get(),frame);
            if(floatCase) {
                const auto currentSource=before_sr_float::Read(device.Get(),context.Get(),inputs[2].Get());
                Check(currentSource.bytes==floatSource->bytes,"BeforeSR must preserve every original source byte");
                const auto currentHistory=before_sr_float::Read(device.Get(),context.Get(),history.Get());
                Check(currentHistory.GreenHistory(),"BeforeSR must preserve the original full auxiliary MRT/history");
                const auto currentOutput=before_sr_float::Read(device.Get(),context.Get(),target.Get());
                const auto expected=before_sr_float::Expected(*floatSource);
                const bool matches=currentOutput.Interior(expected,.035f);
                floatConsecutive=matches?floatConsecutive+1:0;
                if(floatConsecutive==4) {
                    const auto value=currentOutput.At(output/2,output/2);
                    std::printf("PASS float phase format=%u phase=%u current=%.6f,%.6f,%.6f expected=%.6f,%.6f,%.6f consecutive=%u source_bytes=%zu\n",unsigned(floatCase->format),floatPhase,value[0],value[1],value[2],expected[0],expected[1],expected[2],floatConsecutive,currentSource.bytes.size());
                    ++floatPhase;floatVerified+=floatConsecutive;floatConsecutive=0;
                } else if(!matches)Sleep(10);
                continue;
            }
            if(frame>=6) {
                // These explicit readbacks are fixture assertions, never a
                // production wait or a substitute for the slot's own event.
                const auto out=read(target.Get()),aux=read(history.Get());
                ++checked;
                if(continuity&&(absent||frame==32||jitterGap||frame==45||consumerGap))++continuityGap;
                if(pattern) {
                    const std::array<std::array<float,2>,4> points{{{.5f,.12f},{.5f,.88f},{.12f,.5f},{.88f,.5f}}};
                    const std::array<float,4> expected{.125f,.5f,.375f,.625f};
                    for(unsigned i=0;i<4;++i) {
                        const auto source=read(inputs[2].Get(),points[i][0],points[i][1]);
                        Check(std::abs(source[0]-expected[i])<.001f,"directional source rows remain in the original colour domain");
                        lastDirections[i]=read(presentTexture.Get(),points[i][0],points[i][1])[0];
                        Check(std::isfinite(lastDirections[i]),"final directional pixels are finite");
                    }
                    const float top=lastDirections[patternFlip?1:0],bottom=lastDirections[patternFlip?0:1];
                    if(!(top+.04f<lastDirections[2]&&lastDirections[2]+.04f<bottom&&bottom+.04f<lastDirections[3]))
                        std::fprintf(stderr,"directional mismatch frame=%u top=%f bottom=%f left=%f right=%f flip=%d replay=%d\n",frame,lastDirections[0],lastDirections[1],lastDirections[2],lastDirections[3],patternFlip,postReplay);
                    Check(top+.04f<lastDirections[2]&&lastDirections[2]+.04f<bottom&&bottom+.04f<lastDirections[3],"physical full-size consumer preserves source direction and actual shader UV convention");
                    ++directionalFrames;
                }
                if(fullsize&&std::isfinite(delivered[0])&&delivered[0]>.05f&&delivered[0]<(matrixPost?.9f:.7f)&&std::abs(delivered[0]-delivered[1])<.03f&&std::abs(delivered[0]-delivered[2])<.03f)++deliveredGray;
                if(std::isfinite(out[0])&&out[0]>.05f&&out[0]<.7f&&std::abs(out[0]-out[1])<.03f&&std::abs(out[0]-out[2])<.03f)++gray;
                if(sourceReturn && std::isfinite(delivered[0]) && delivered[0]>.05f && delivered[0]<.7f && std::abs(delivered[0]-delivered[1])<.03f) {
                    if(frame<24)++sourceBefore;
                    if(frame>=54)++sourceAfter;
                }
                if(sourceReturn&&frame>=48&&frame<54&&std::isfinite(out[0])&&out[0]>.05f&&out[0]<.7f&&std::abs(out[0]-out[1])<.03f&&std::abs(out[0]-out[2])<.03f)++recoveryLearning;
                if(out[0]>.95f&&out[1]<.05f&&out[2]>.95f)++magenta;
                if(aux[0]==0&&aux[1]==1&&aux[2]==0)++greenHistory;
                if(alternate&&frame==8&&out[0]==1&&out[1]==0&&out[2]==1)++rejectedJitter;
            }
        }
        if(observationJoined) {
            const auto directory=std::filesystem::current_path()/L"taa-shaders"/std::to_wstring(GetCurrentProcessId());
            const auto field=[](const std::string& text,const std::string& key) {
                const auto at=text.find(key+"=");Check(at!=std::string::npos,"complete observation identity field");
                return std::stoull(text.substr(at+key.size()+1));
            };
            std::map<uint64_t,std::string> rasters;std::vector<std::string> joined;unsigned seeds=0;
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            for(;;) {
                rasters.clear();joined.clear();seeds=0;
                for(const auto* kind:{L"raster",L"temporal"})if(std::filesystem::exists(directory/kind))
                    for(const auto& entry:std::filesystem::directory_iterator(directory/kind)) {
                        if(!entry.path().wstring().ends_with(L".meta.txt"))continue;
                        std::ifstream stream(entry.path());const std::string text((std::istreambuf_iterator<char>(stream)),{});
                        if(kind==std::wstring_view(L"raster"))rasters.emplace(field(text,"raster_record"),text);
                        else if(entry.path().filename().wstring().starts_with(L"PS-B12FF59247A26796")) {
                            if(text.find("capture_selection=seed")!=std::string::npos)++seeds;
                            if(text.find("capture_selection=same_owner_joined")!=std::string::npos)joined.push_back(text);
                        }
                    }
                if(seeds==1&&joined.size()>=4&&rasters.size()>=joined.size())break;
                Check(std::chrono::steady_clock::now()<deadline,"actual content-identified B12 seed and joined records complete");Sleep(10);
            }
            Check(seeds+joined.size()<=8,"seed and same-owner joins share the original eight sample slots");
            std::map<uint64_t,bool> unknownRecords;
            const std::regex reference("joined_raster_record=([0-9]+)");
            for(const auto& temporal:joined) {
                const auto inputAt=temporal.find("\nt1 resource=");
                Check(inputAt!=std::string::npos,"B12 resolve records its actual motion input resource");
                const auto input=temporal.substr(inputAt+1,temporal.find('\n',inputAt+1)-inputAt-1);
                unsigned motionJoins=0;
                for(std::sregex_iterator item(temporal.begin(),temporal.end(),reference),end;item!=end;++item) {
                    const auto record=std::stoull((*item)[1].str());const auto found=rasters.find(record);
                    Check(found!=rasters.end(),"every resolve join references an exact successfully queued raster export");
                    const auto& raster=found->second;
                    for(const auto* key:{"owner_frame","owner_epoch","config_revision","context_id","thread_id","capture_epoch","capture_burst"})
                        Check(field(raster,key)==field(temporal,key),"actual raster and B12 resolve share every owner/capture identity");
                    const auto at=size_t(item->position());
                    const auto line=temporal.substr(at,temporal.find('\n',at)-at);
                    if(line.find("role=motion_hint")!=std::string::npos) {
                        Check(field(line,"input_slot")==1&&field(line,"output_slot")==1&&field(line,"resource")==field(input,"resource"),
                              "actual B12 motion join binds its t1 resource to the real MRT1 writer");
                        const auto motionTarget="role=motion_hint resource="+std::to_string(field(input,"resource"))+" input_slot=1 output_slot=1 ";
                        Check(raster.find(motionTarget)!=std::string::npos,"the exact raster receipt exports the joined MRT1 motion target");
                        ++motionJoins;
                    }
                    if(raster.find("known_bytes=0")!=std::string::npos)unknownRecords[record]=true;
                }
                Check(motionJoins>=1,"every joined B12 resolve has its own exact motion-writer receipt");
            }
            const auto unknown=unsigned(unknownRecords.size());
            Check(unknown>=4,"multiple same-owner B12 joins survive fully unknown producer constants and resource rotation");
            std::ifstream saved(directory/L"PS-B12FF59247A26796.dxbc",std::ios::binary);const std::vector<char> bytes((std::istreambuf_iterator<char>(saved)),{});
            Check(bytes.size()==pixel->GetBufferSize()&&!std::memcmp(bytes.data(),pixel->GetBufferPointer(),bytes.size()),"full captured B12 bytecode is exported unchanged");
            std::printf("PASS exact asynchronous TAA shader evidence\nPASS actual B12 seed and same-owner raster joins: seed=%u joined=%zu unknown=%u\n",seeds,joined.size(),unknown);
            return 0;
        }
        if(floatCase) {
            Check(floatPhase==3&&floatVerified==12,"actual float BeforeSR must reach every current-frame oracle before its deadline");
            std::printf("PASS independent TAA BeforeSR float source values: format=%u phases=%u verified=%u source_unchanged=true auxiliary_unchanged=true\n",unsigned(floatCase->format),floatPhase,floatVerified);
            std::printf("PASS actual F6 TAA Hook and provider pixels\n");ExitProcess(0);
        }
        const auto out=read(target.Get()),aux=read(history.Get());
        std::printf("TAA hook pixels: replacing=%d compute=%d render=%u output=%u rgb=%.5f,%.5f,%.5f auxiliary=%.5f,%.5f,%.5f\n",replacing,compute,render,output,out[0],out[1],out[2],aux[0],aux[1],aux[2]);
        std::printf("TAA hook continuous frames: checked=%u gray=%u original=%u auxiliary=%u\n",checked,gray,magenta,greenHistory);
        Check(checked==frames-6&&greenHistory==checked,"original auxiliary history must remain green on every checked frame");
        if(alternate) Check(rejectedJitter==1&&magenta==1&&gray+1==checked,"state switches must observe invalid jitter and recover on the next valid write");
        else if(autoSwitch)Check(gray>=checked-3&&out[0]<.7f,"compatible automatic adapters retain the provider and recover real output");
        else if(continuity) {
            Check(continuityGap==11+(fullsize?1u:0u),"all deliberate source and consumer gaps were exercised");
            if(fullsize)Check(deliveredGray==checked-continuityGap&&magenta==checked,"only each current valid consumer receives full-resolution SR");
            else Check(gray==checked-continuityGap&&magenta==continuityGap,"invalid inputs are not evaluated or replaced; recovery uses current pixels");
            std::printf("PASS stable feature through temporal gaps: checked=%u rejected=%u fullsize=%d\n",checked,continuityGap,fullsize);
        }
        else if(sourceReturn) {
            // Returning from Present NR explicitly relearns the consumer route.
            // Only those six recovery frames may copy current SR into the low target.
            Check(sourceBefore==18&&sourceAfter==18&&recoveryLearning==0&&magenta==checked,"full-size source recovers after Present fallback without rebuilding or low-target relearning");
            std::printf("PASS actual source -> Present fresh colour -> source pixels: before=%u after=%u original=%u current_learning=%u\n",sourceBefore,sourceAfter,magenta,recoveryLearning);
        }
        else if(opaqueConsumer) {Check(gray==checked,"opaque submissions retain original-target SR rather than permanently withholding it");std::printf("PASS opaque consumer cancellation retains current target output: frames=%u\n",gray);}
        else if(fullsize) {if(pattern){Check(directionalFrames==checked,"directional output remains current on every measured frame");std::printf("PASS directional source and final consumer pixels: frames=%u top=%f bottom=%f left=%f right=%f flip=%d replay=%d\n",directionalFrames,lastDirections[0],lastDirections[1],lastDirections[2],lastDirections[3],patternFlip,postReplay);}Check(deliveredGray==checked&&magenta==checked,"actual consumer gets full-size SR while original low TAA remains untouched");std::printf("PASS actual full-size consumer pixels: render=%u output=%u delivered=%u replay=%d static_default_vb=true\n",render,present,deliveredGray,postReplay);}
        else if(replacing) Check(gray==checked,"actual TAA Hook -> selected SR -> gray copyback on every checked frame");
        else Check(magenta==checked,"off/discover/no-output must preserve original magenta on every checked frame");
        if(matrixPost){Check(matrixVerified==checked&&matrixMismatches==0,"every measured exact matrix/postprocess frame matched the independent oracle");std::printf("PASS exact PS6CA VSF1CE matrix and multi-texture oracle: frames=%u noise_branch=true\n",matrixVerified);}
        if(projection)std::printf("PASS actual indexed projection source without resolve jitter: positive=%d frames=%u gray=%u original=%u\n",projectionPositive,checked,gray,magenta);
        if(automatic) {
            std::wostringstream name;name<<(compute?L"CS-":L"PS-")<<std::uppercase<<std::hex<<std::setw(16)<<std::setfill(L'0')<<hash;
            const auto stem=std::filesystem::current_path()/L"taa-shaders"/std::to_wstring(GetCurrentProcessId())/name.str();
            auto metadata=stem;metadata+=L".meta.txt";
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            while(!std::filesystem::exists(metadata)&&std::chrono::steady_clock::now()<deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            auto dump=stem;dump+=L".dxbc";
            std::ifstream input(dump,std::ios::binary);std::vector<char> saved((std::istreambuf_iterator<char>(input)),{});
            Check(std::filesystem::exists(metadata)&&saved.size()==selected->GetBufferSize(),"asynchronous evidence includes an exact complete DXBC");
            Check(std::memcmp(saved.data(),selected->GetBufferPointer(),saved.size())==0,"evidence preserves complete shader bytes and fingerprint identity");
            ComPtr<ID3DBlob> disassembly;Hr(D3DDisassemble(saved.data(),saved.size(),0,nullptr,&disassembly));
            std::ifstream report(metadata);const std::string text((std::istreambuf_iterator<char>(report)),{});
            Check(text.find("candidate=")!=std::string::npos&&text.find("bindings=")!=std::string::npos,"shader evidence includes source bindings and candidate identity");
            if(unsupported)Check(text.find("Temporal shader uses unsupported control flow")!=std::string::npos&&text.find("unsupported_instruction=discard")!=std::string::npos,"unsupported candidates also publish their exact analysis rejection and instruction");
            std::printf("PASS exact asynchronous TAA shader evidence\n");
            if(noJitter||mode==L"auto-pixel"||mode==L"auto-compute") {
                const auto root=stem.parent_path();std::filesystem::path frameReport;
                const auto timeout=std::chrono::steady_clock::now()+std::chrono::seconds(5);
                while(frameReport.empty()&&std::chrono::steady_clock::now()<timeout) {
                    if(std::filesystem::exists(root/L"temporal"))for(const auto& entry:std::filesystem::directory_iterator(root/L"temporal")) {
                        const auto filename=entry.path().filename().wstring();
                        if(!filename.starts_with(name.str())||!filename.ends_with(L".meta.txt"))continue;
                        std::ifstream source(entry.path());const std::string record((std::istreambuf_iterator<char>(source)),{});
                        if(record.find("known_bytes=16")!=std::string::npos) {frameReport=entry.path();break;}
                    }
                    if(frameReport.empty())std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                Check(!frameReport.empty(),"actual hook exports observed companion CB writes even when jitter is absent");
                auto binary=frameReport.wstring();binary.resize(binary.size()-std::wstring(L".meta.txt").size());binary+=L".constants.bin";
                std::ifstream captured(std::filesystem::path(binary),std::ios::binary);const std::vector<char> content((std::istreambuf_iterator<char>(captured)),{});
                Check(content.size()==32,"actual companion file contains exact CB bytes plus known mask");
                float x=0;std::memcpy(&x,content.data()+8,4);
                Check(x==.25f/float(render),"actual companion file records current CPU values and does not zero-fill known jitter bytes");
                for(unsigned i=16;i<32;++i)Check(content[i]==1,"actual companion file marks the complete observed write");
                if(!compute) {
                    const auto vertexHash=DrawObservationDx11::ShaderByteHash(vsCode->GetBufferPointer(),vsCode->GetBufferSize());
                    std::wostringstream vertexName;vertexName<<L"VS-"<<std::uppercase<<std::hex<<std::setw(16)<<std::setfill(L'0')<<vertexHash<<L".dxbc";
                    std::ifstream vertex(root/vertexName.str(),std::ios::binary);const std::vector<char> code((std::istreambuf_iterator<char>(vertex)),{});
                    Check(code.size()==vsCode->GetBufferSize()&&std::memcmp(code.data(),vsCode->GetBufferPointer(),code.size())==0,"actual hook exports the exact bound companion vertex shader");
                }
                std::printf("PASS actual companion VS and observed constants\n");
            }
            if(rasterEvidence) {
                const auto root=stem.parent_path();const auto rasterDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
                std::vector<std::tuple<uint64_t,unsigned,unsigned>> stamps;
                std::array<unsigned,9> burstCounts{};
                const auto sceneHash=DrawObservationDx11::ShaderByteHash(sceneVsCode->GetBufferPointer(),sceneVsCode->GetBufferSize());
                while((stamps.size()<4||(timedRaster&&*std::min_element(burstCounts.begin(),burstCounts.begin()+4)<4))&&std::chrono::steady_clock::now()<rasterDeadline) {
                    stamps.clear();burstCounts.fill(0);
                    if(std::filesystem::exists(root/L"raster"))for(const auto& entry:std::filesystem::directory_iterator(root/L"raster")) {
                        if(!entry.path().wstring().ends_with(L".meta.txt"))continue;
                        std::ifstream rasterReport(entry.path());const std::string rasterText((std::istreambuf_iterator<char>(rasterReport)),{});
                        if(rasterText.find("known_bytes=16")==std::string::npos)continue;
                        Check(rasterText.find("draw_kind=4")!=std::string::npos&&rasterText.find("instances=2")!=std::string::npos&&rasterText.find("completion=original_returned")!=std::string::npos,"raster evidence includes the actual indexed instanced draw completion");
                        Check(rasterText.find("role=color")!=std::string::npos&&rasterText.find("VS_cb2 ")!=std::string::npos,"raster evidence follows the observed TAA color resource and scene VS constant binding");
                        const auto burstPosition=rasterText.find("capture_burst=");Check(burstPosition!=std::string::npos,"scene evidence binds a timed burst");
                        const auto burst=unsigned(std::stoul(rasterText.substr(burstPosition+14)));Check(burst<burstCounts.size()&&(extentRaster||burst<4)&&(!burst||timedRaster),"scene evidence uses the expected capture window");++burstCounts[burst];
                        Check(rasterText.find("write_serial=0")==std::string::npos&&rasterText.find("write_serial=")!=std::string::npos,"scene evidence binds a real CPU write identity");
                        auto binary=entry.path().wstring();binary.resize(binary.size()-std::wstring(L".meta.txt").size());binary+=L".constants.bin";
                        std::ifstream rasterInput(std::filesystem::path(binary),std::ios::binary);const std::vector<char> bytes((std::istreambuf_iterator<char>(rasterInput)),{});
                        Check(bytes.size()==32,"scene VS exports exact constants and known mask");std::array<float,4> values{};std::memcpy(values.data(),bytes.data(),16);
                        Check(values[2]>=1&&values[2]<=frames&&values[0]==values[2]*.00001f&&values[1]==-values[2]*.00002f&&values[3]==1234.5f,"scene constants preserve all values from the same stamped CPU write");
                        const auto position=rasterText.find("owner_frame=");Check(position!=std::string::npos,"scene record includes its draw owner frame");
                        stamps.emplace_back(std::stoull(rasterText.substr(position+12)),unsigned(values[2]),burst);
                        for(size_t i=16;i<32;++i)Check(bytes[i]==1,"scene VS constant bytes are observed");
                    }
                    if(stamps.size()<4)std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                Check(stamps.size()>=4,"actual DLL records a sequence of linked scene VS constants");
                if(warmupRaster) {
                    for(const auto& stamp:stamps)Check(std::get<1>(stamp)>=14,"late producer known constants come from writes after watch attachment");
                    std::printf("PASS actual delayed producer after twelve unrelated owner frames with CPU constant warmup\n");
                }
                std::sort(stamps.begin(),stamps.end());for(size_t i=1;i<stamps.size();++i)
                    if(std::get<2>(stamps[i])==std::get<2>(stamps[i-1]))Check(std::get<0>(stamps[i])==std::get<0>(stamps[i-1])+1&&std::get<1>(stamps[i])==std::get<1>(stamps[i-1])+1,"consecutive owner frames use consecutive current CPU writes");
                if(timedRaster) {
                    Check(*std::min_element(burstCounts.begin(),burstCounts.begin()+4)>=4,"actual DLL captures consecutive scene writes in all four timed bursts");
                    std::printf("PASS actual four timed capture bursts: %u,%u,%u,%u\n",burstCounts[0],burstCounts[1],burstCounts[2],burstCounts[3]);
                }
                if(extentRaster) {
                    Check(burstCounts[4]>=4&&burstCounts[8]>=4,"late extent changes reattach actual scene evidence twice");
                    Check(burstCounts[4]<=8&&burstCounts[8]<=8,"late scene capture remains bounded");
                    std::printf("PASS actual late extent capture bursts: %u,%u\n",burstCounts[4],burstCounts[8]);
                }
                std::wostringstream sceneName;sceneName<<L"VS-"<<std::uppercase<<std::hex<<std::setw(16)<<std::setfill(L'0')<<sceneHash<<L".dxbc";
                std::ifstream source(root/sceneName.str(),std::ios::binary);const std::vector<char> bytes((std::istreambuf_iterator<char>(source)),{});
                Check(bytes.size()==sceneVsCode->GetBufferSize()&&std::memcmp(bytes.data(),sceneVsCode->GetBufferPointer(),bytes.size())==0,"raster export is the exact scene VS instead of the resolve fullscreen VS");
                std::printf("PASS linked indexed instanced raster VS and consecutive observed constants\n");
            }
        }
        std::printf("PASS actual F6 TAA Hook and provider pixels\n");
        // Process teardown owns remaining SDK/plugin roots, as in the existing
        // receipt host. Never unload a live hooked DLL from this fixture.
        ExitProcess(0);
    } catch(const std::exception& e) {std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
