#include <inputs/taa/TaaPassDx11.h>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <limits>
#include <d3d11sdklayers.h>

using namespace taa;
namespace {
unsigned checks=0;
void Check(bool ok,const char* label) {++checks;if(!ok)throw std::runtime_error(label);}
void Hr(HRESULT hr) {Check(SUCCEEDED(hr),"D3D11 fixture API failed");}
ComPtr<ID3DBlob> Shader(const char* source,const char* target) {
    ComPtr<ID3DBlob> code,errors;
    const auto hr=D3DCompile(source,std::strlen(source),"TAA fixture",nullptr,nullptr,"main",target,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors);
    if(FAILED(hr)&&errors)std::fprintf(stderr,"%s",static_cast<const char*>(errors->GetBufferPointer()));
    Hr(hr);return code;
}
struct Fixture {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11DeviceContext1> context1;
    ComPtr<ID3D11InfoQueue> debug;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11ComputeShader> cs;
    ComPtr<ID3D11DepthStencilState> noDepth;
    ComPtr<ID3D11Buffer> jitter;
    std::array<ComPtr<ID3D11Texture2D>,3> sources;
    std::array<ComPtr<ID3D11ShaderResourceView>,3> srvs;
    ComPtr<ID3D11Texture2D> target,aux,sentinel;
    ComPtr<ID3D11RenderTargetView> targetRtv,auxRtv;
    ComPtr<ID3D11UnorderedAccessView> targetUav,auxUav;
    Profile profile;
    unsigned outputSize=16;
    std::array<float,256> cb{};
    Fixture() {
        Hr(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
        Hr(context.As(&context1));Hr(device.As(&debug));
        D3D11_DEPTH_STENCIL_DESC ds{};ds.DepthFunc=D3D11_COMPARISON_ALWAYS;Hr(device->CreateDepthStencilState(&ds,&noDepth));
        D3D11_BUFFER_DESC bd{};bd.ByteWidth=sizeof(cb);bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;bd.Usage=D3D11_USAGE_DYNAMIC;bd.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
        Hr(device->CreateBuffer(&bd,nullptr,&jitter));
        auto v=Shader("float4 main(uint id:SV_VertexID):SV_Position{return float4(float2((id<<1)&2,id&2)*float2(2,-2)+float2(-1,1),0,1);}","vs_5_0");
        Hr(device->CreateVertexShader(v->GetBufferPointer(),v->GetBufferSize(),nullptr,&vs));
        auto p=Shader("Texture2D<float4> color:register(t2);struct O{float4 a:SV_Target0;float4 b:SV_Target1;};O main(float4 pos:SV_Position){O o;o.a=color.Load(int3(uint2(pos.xy)%16,0));o.b=float4(.3,.4,.5,1);return o;}","ps_5_0");
        Hr(device->CreatePixelShader(p->GetBufferPointer(),p->GetBufferSize(),nullptr,&ps));
        auto c=Shader("Texture2D<float4> color:register(t2);RWTexture2D<float4> output:register(u0);RWTexture2D<float4> aux:register(u1);[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID){output[p.xy]=color.Load(int3(p.xy%16,0));aux[p.xy]=float4(.3,.4,.5,1);}","cs_5_0");
        Hr(device->CreateComputeShader(c->GetBufferPointer(),c->GetBufferSize(),nullptr,&cs));
        const std::array<float,4> color{.1f,.2f,.3f,1},depth{.75f,0,0,0},motion{.125f,-.25f,0,0};
        sources[0]=Texture(16,DXGI_FORMAT_R32G32B32A32_FLOAT,color,4);
        sources[1]=Texture(16,DXGI_FORMAT_R32_FLOAT,depth,1);sources[2]=Texture(16,DXGI_FORMAT_R32G32_FLOAT,motion,2);
        for(unsigned i=0;i<3;++i)Hr(device->CreateShaderResourceView(sources[i].Get(),nullptr,&srvs[i]));
        Resize(16);
        profile.shaderHash=1;profile.colorSlot=2;profile.depthSlot=0;profile.motionSlot=1;
        profile.jitterSlot=0;profile.jitterOffsetX=8;profile.jitterOffsetY=12;
        profile.jitterScaleX=profile.jitterScaleY=1;profile.motionScaleX=2;profile.motionScaleY=-3;
        profile.motionUnits=Units::Uv;profile.threadsX=profile.threadsY=8;
        Bind();
    }
    ComPtr<ID3D11Texture2D> Texture(unsigned size,DXGI_FORMAT format,const std::array<float,4>& values,unsigned channels) {
        std::vector<float> data(size*size*channels);
        for(size_t i=0;i<data.size();++i)data[i]=values[i%channels];
        D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=size;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
        d.Format=format;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET|D3D11_BIND_UNORDERED_ACCESS;
        D3D11_SUBRESOURCE_DATA initial{data.data(),size*channels*UINT(sizeof(float)),0};
        ComPtr<ID3D11Texture2D> result;Hr(device->CreateTexture2D(&d,&initial,&result));return result;
    }
    void Resize(unsigned size) {
        context->ClearState();targetRtv.Reset();auxRtv.Reset();targetUav.Reset();auxUav.Reset();
        outputSize=size;target=Texture(size,DXGI_FORMAT_R32G32B32A32_FLOAT,{0,0,0,0},4);
        aux=Texture(size,DXGI_FORMAT_R32G32B32A32_FLOAT,{0,0,0,0},4);
        sentinel=Texture(size,DXGI_FORMAT_R32G32B32A32_FLOAT,{.8f,.6f,.4f,1},4);
        Hr(device->CreateRenderTargetView(target.Get(),nullptr,&targetRtv));Hr(device->CreateRenderTargetView(aux.Get(),nullptr,&auxRtv));
        Hr(device->CreateUnorderedAccessView(target.Get(),nullptr,&targetUav));Hr(device->CreateUnorderedAccessView(aux.Get(),nullptr,&auxUav));
    }
    void Bind() {
        context->ClearState();
        ID3D11ShaderResourceView* views[]{srvs[1].Get(),srvs[2].Get(),srvs[0].Get()};
        ID3D11Buffer* buffer=jitter.Get();UINT first=16,count=16;
        if(profile.stage==Stage::Pixel) {
            ID3D11RenderTargetView* rtvs[]{targetRtv.Get(),auxRtv.Get()};context->OMSetRenderTargets(2,rtvs,nullptr);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);
            context->OMSetDepthStencilState(noDepth.Get(),0);
            context->PSSetShaderResources(0,3,views);context1->PSSetConstantBuffers1(0,1,&buffer,&first,&count);
            D3D11_VIEWPORT vp{0,0,float(outputSize),float(outputSize),0,1};context->RSSetViewports(1,&vp);
        } else {
            context->CSSetShader(cs.Get(),nullptr,0);context->CSSetShaderResources(0,3,views);
            context1->CSSetConstantBuffers1(0,1,&buffer,&first,&count);
            ID3D11UnorderedAccessView* uavs[]{targetUav.Get(),auxUav.Get()};context->CSSetUnorderedAccessViews(0,2,uavs,nullptr);
        }
    }
    void Jitter(float x=.25f,float y=-.5f) {
        cb[66]=x;cb[67]=y;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const auto hr=context->Map(jitter.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped);Hr(hr);
        constants::Mapped(context.Get(),jitter.Get(),0,D3D11_MAP_WRITE_DISCARD,&mapped,hr);
        std::memcpy(mapped.pData,cb.data(),sizeof(cb));constants::BeforeUnmap(context.Get(),jitter.Get(),0);
        context->Unmap(jitter.Get(),0);constants::AfterUnmap(context.Get(),jitter.Get(),0);
    }
    std::array<float,4> Read(ID3D11Texture2D* resource) {
        D3D11_TEXTURE2D_DESC d{};resource->GetDesc(&d);d.Usage=D3D11_USAGE_STAGING;d.BindFlags=d.MiscFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;Hr(device->CreateTexture2D(&d,nullptr,&staging));context->CopyResource(staging.Get(),resource);
        D3D11_MAPPED_SUBRESOURCE data{};Hr(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&data));
        std::array<float,4> value{};const unsigned n=d.Format==DXGI_FORMAT_R32_FLOAT?1:d.Format==DXGI_FORMAT_R32G32_FLOAT?2:4;
        std::memcpy(value.data(),data.pData,n*sizeof(float));context->Unmap(staging.Get(),0);return value;
    }
    Invocation Call() const {return {profile.stage,3,1,outputSize/8,outputSize/8,1,true};}
    void Original() {if(profile.stage==Stage::Pixel)context->Draw(3,0);else context->Dispatch(outputSize/8,outputSize/8,1);}
    void Drain(PassDx11& pass) {context->Flush();for(unsigned i=0;i<5000&&!pass.Idle();++i)std::this_thread::sleep_for(std::chrono::milliseconds(1));Check(pass.Idle(),"actual context completion event");}
    void CheckState() {
        ComPtr<ID3D11Buffer> buffer;UINT first=0,count=0;
        if(profile.stage==Stage::Pixel) {
            context1->PSGetConstantBuffers1(0,1,&buffer,&first,&count);
            ComPtr<ID3D11PixelShader> actual;context->PSGetShader(&actual,nullptr,nullptr);Check(actual.Get()==ps.Get(),"PS restored");
            ComPtr<ID3D11RenderTargetView> targetView;context->OMGetRenderTargets(1,&targetView,nullptr);Check(targetView.Get()==targetRtv.Get(),"RTV restored");
        } else {
            context1->CSGetConstantBuffers1(0,1,&buffer,&first,&count);
            ComPtr<ID3D11ComputeShader> actual;context->CSGetShader(&actual,nullptr,nullptr);Check(actual.Get()==cs.Get(),"CS restored");
            ComPtr<ID3D11UnorderedAccessView> targetView;context->CSGetUnorderedAccessViews(0,1,&targetView);Check(targetView.Get()==targetUav.Get(),"UAV restored");
        }
        Check(buffer.Get()==jitter.Get()&&first==16&&count==16,"CB1 identity and ranges restored");
    }
};
void DepthStencilChecks(Fixture& f,PassDx11& pass) {
    f.profile.stage=Stage::Pixel;f.Resize(16);
    const auto pixels=[&](ID3D11Texture2D* source) {
        D3D11_TEXTURE2D_DESC d{};source->GetDesc(&d);d.BindFlags=d.MiscFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;Hr(f.device->CreateTexture2D(&d,nullptr,&staging));f.context->CopyResource(staging.Get(),source);
        D3D11_MAPPED_SUBRESOURCE mapped{};Hr(f.context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));
        const unsigned row=d.Width*(d.Format==DXGI_FORMAT_D24_UNORM_S8_UINT?4:16);std::vector<unsigned char> data(size_t(row)*d.Height);
        for(unsigned y=0;y<d.Height;++y)std::memcpy(data.data()+size_t(y)*row,static_cast<const unsigned char*>(mapped.pData)+size_t(y)*mapped.RowPitch,row);
        f.context->Unmap(staging.Get(),0);return data;
    };
    // Reuse the same shader while switching attachment and state on adjacent
    // draws. Neither shader nor previous-frame coverage may authorize this one.
    for(unsigned mode=0;mode<12;++mode) {
        f.Bind();f.Jitter();
        const bool bound=mode>=3&&mode!=11,accept=mode<7||mode==11;
        D3D11_DEPTH_STENCIL_DESC d{};d.DepthFunc=D3D11_COMPARISON_ALWAYS;d.StencilReadMask=d.StencilWriteMask=255;
        d.FrontFace={D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_COMPARISON_ALWAYS};d.BackFace=d.FrontFace;
        if(mode==1||mode==4||mode==5||mode==7||mode==8){d.DepthEnable=TRUE;d.DepthWriteMask=mode==5||mode==7||mode==8?D3D11_DEPTH_WRITE_MASK_ZERO:D3D11_DEPTH_WRITE_MASK_ALL;}
        if(mode==1||mode==7)d.DepthFunc=D3D11_COMPARISON_LESS;
        if(mode==8)d.DepthFunc=D3D11_COMPARISON_EQUAL;
        if(mode==2||mode==6||mode==9||mode==10){d.StencilEnable=TRUE;d.StencilWriteMask=mode==6?255:0;}
        if(mode==2||mode==9)d.FrontFace.StencilFunc=d.BackFace.StencilFunc=D3D11_COMPARISON_EQUAL;
        if(mode==2)d.StencilReadMask=0; // Zero mask is not an unconditional function.
        if(mode==6)d.FrontFace.StencilPassOp=d.BackFace.StencilPassOp=D3D11_STENCIL_OP_INCR_SAT;
        if(mode==10)d.BackFace.StencilFunc=D3D11_COMPARISON_NEVER;
        ComPtr<ID3D11DepthStencilState> state;Hr(f.device->CreateDepthStencilState(&d,&state));
        f.context->OMSetDepthStencilState(mode==0||mode==11?nullptr:state.Get(),1);
        std::array<UINT,256> packed{};for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)packed[y*16+x]=(x<8?0u:0x00ffffffu)|(x<8?0u:0x01000000u);
        D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=16;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;td.BindFlags=D3D11_BIND_DEPTH_STENCIL;
        D3D11_SUBRESOURCE_DATA init{packed.data(),16*4,0};ComPtr<ID3D11Texture2D> texture;Hr(f.device->CreateTexture2D(&td,&init,&texture));
        D3D11_DEPTH_STENCIL_VIEW_DESC vd{};vd.Format=td.Format;vd.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
        if(mode==7||mode==8)vd.Flags=D3D11_DSV_READ_ONLY_DEPTH|D3D11_DSV_READ_ONLY_STENCIL;
        ComPtr<ID3D11DepthStencilView> view;Hr(f.device->CreateDepthStencilView(texture.Get(),&vd,&view));
        ID3D11RenderTargetView* targets[]{f.targetRtv.Get(),f.auxRtv.Get()};f.context->OMSetRenderTargets(2,targets,bound?view.Get():nullptr);
        const float clear[]{.05f,.15f,.25f,1};f.context->ClearRenderTargetView(f.targetRtv.Get(),clear);f.context->ClearRenderTargetView(f.auxRtv.Get(),clear);
        const bool began=pass.Begin(f.profile,f.Call(),true);Check(began==accept,"effective DS coverage admission");
        if(!accept)Check(pass.Error()==(mode==7||mode==8?"depth-tested TAA draw":"stencil-tested TAA draw"),"specific bound-test rejection");
        f.Original();unsigned evaluations=0;
        if(began)Check(pass.Finish([&](const Frame& frame){++evaluations;f.context->CopyResource(frame.output,f.sentinel.Get());return true;}),"DS-equivalent SR copyback");
        Check(evaluations==unsigned(accept),"tested pixels never reach SR evaluation");
        ComPtr<ID3D11DepthStencilView> actual;f.context->OMGetRenderTargets(0,nullptr,&actual);Check(actual.Get()==(bound?view.Get():nullptr),"actual DSV restored");
        ComPtr<ID3D11DepthStencilState> actualState;UINT ref=0;f.context->OMGetDepthStencilState(&actualState,&ref);Check(actualState.Get()==(mode==0||mode==11?nullptr:state.Get())&&ref==1,"DS state and stencil reference restored");
        const auto color=pixels(f.target.Get()),aux=pixels(f.aux.Get()),ds=pixels(texture.Get());
        for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
            const unsigned i=y*16+x;const bool covered=mode==7||mode==9?x>=8:mode==8?x<8:true;
            float c=0,a=0;std::memcpy(&c,color.data()+size_t(i)*16,4);std::memcpy(&a,aux.data()+size_t(i)*16,4);
            Check(std::abs(c-(accept?.8f:covered?.1f:.05f))<1e-5f,"all current color pixels preserve DS coverage");
            Check(std::abs(a-(covered?.3f:.05f))<1e-5f,"all original auxiliary pixels preserve DS coverage");
            UINT stored=0;std::memcpy(&stored,ds.data()+size_t(i)*4,4);
            const UINT expected=mode==4?(packed[i]&0xff000000u):mode==6?packed[i]+0x01000000u:packed[i];
            Check(stored==expected,"original depth/stencil side effects preserved exactly once");
        }
        if(began)f.Drain(pass);
    }
}
void ProfileChecks() {
    Check(ParseMode("")==Mode::Off&&ParseMode("auto")==Mode::Off,"old INI defaults off");
    Check(ParseMode("rEpLaCe")==Mode::Replace&&!ParseMode("oops"),"mode parsing");
    CSimpleIniA ini;
    const char* text="[TAA.Profile]\nSchema=1\nStage=pixel\nShaderHash=0000000000000001\nColorSlot=2\nDepthSlot=0\nMotionSlot=1\nOutputSlot=0\nJitterSlot=0\nJitterOffsetX=8\nJitterOffsetY=12\nMotionEncoding=linear\nMotionUnits=uv\nMotionScaleX=1\nMotionScaleY=-1\nJitterUnits=pixels\nJitterScaleX=1\nJitterScaleY=-1\nMotionJittered=false\nDepthInverted=true\nHDR=false\nHistory=preserve-original\n";
    Check(ini.LoadData(text)>=0&&ReadProfile(ini).profile.has_value(),"complete production profile parser");
    Check(ReadProfile(ini).profile->hdrDeclared&&!ReadProfile(ini).profile->hdr,"manual HDR=false is a declared create flag");
    ini.SetValue("TAA.Profile","HDR","true");Check(ReadProfile(ini).profile->hdrDeclared&&ReadProfile(ini).profile->hdr,"manual HDR=true is a declared create flag");
    ini.SetValue("TAA.Profile","HDR","false");
    ini.SetValue("TAA.Profile","MotionSlot","2");Check(!ReadProfile(ini).profile,"aliased slots rejected");
    ini.SetValue("TAA.Profile","MotionSlot","1");ini.SetValue("TAA.Profile","MotionScaleX","nan");Check(!ReadProfile(ini).profile,"nonfinite scale rejected");
    ini.SetValue("TAA.Profile","MotionScaleX","1");ini.SetValue("TAA.Profile","Stage","compute");Check(!ReadProfile(ini).profile,"compute requires group metadata");
    ini.SetValue("TAA.Profile","ThreadsX","8");ini.SetValue("TAA.Profile","ThreadsY","8");Check(ReadProfile(ini).profile.has_value(),"compute profile accepted");
    ini.Delete("TAA.Profile","History");Check(!ReadProfile(ini).profile,"history semantics required");
}
}
int main() {
    try {
        ProfileChecks();Fixture f;PassDx11 pass(f.device.Get(),f.context.Get());
        Check(!pass.Begin(f.profile,f.Call(),true),"no guessed or GPU-readback jitter");
        f.Jitter();
        for(const auto stage:{Stage::Pixel,Stage::Compute})for(const auto size:{16u,32u})for(const bool accept:{false,true}) {
            f.profile.stage=stage;f.Resize(size);f.Bind();f.Jitter();
            Check(pass.Begin(f.profile,f.Call(),!accept),pass.Error().c_str());
            f.Original();
            // Mutating a source after Begin must not change the frozen SR input.
            const auto changed=f.Texture(16,DXGI_FORMAT_R32G32B32A32_FLOAT,{.9f,.9f,.9f,1},4);
            f.context->CopyResource(f.sources[0].Get(),changed.Get());
            const bool result=pass.Finish([&](const Frame& frame) {
                Check(frame.renderWidth==16&&frame.outputWidth==size,"TAA and TAAU extents");
                Check(frame.jitterX==.25f&&frame.jitterY==-.5f&&frame.reset==!accept,"current jitter and reset preserved");
                Check(std::abs(f.Read(frame.color)[0]-.1f)<1e-5f,"pre-TAA color snapshot");
                Check(std::abs(f.Read(frame.depth)[0]-.75f)<1e-5f,"real depth decoded");
                auto motion=f.Read(frame.motion);Check(std::abs(motion[0]-4)<1e-5f&&std::abs(motion[1]-12)<1e-5f,"motion units and axis signs decoded");
                f.context->CopyResource(frame.output,f.sentinel.Get());return accept;
            });
            Check(result==accept,"only current-output proof authorizes replacement");f.CheckState();
            const auto pixels=f.Read(f.target.Get());Check(std::abs(pixels[0]-(accept?.8f:.1f))<1e-5f,"conditional output pixels");
            Check(std::abs(f.Read(f.aux.Get())[0]-.3f)<1e-5f,"original auxiliary history preserved");f.Drain(pass);
            const auto restored=f.Texture(16,DXGI_FORMAT_R32G32B32A32_FLOAT,{.1f,.2f,.3f,1},4);
            f.context->CopyResource(f.sources[0].Get(),restored.Get());
        }
        DepthStencilChecks(f,pass);
        f.profile.stage=Stage::Compute;f.Bind();f.Jitter();
        auto partial=f.Call();partial.groupsX=1;Check(!pass.Begin(f.profile,partial,false),"partial dispatch rejected");
        constants::OpaqueWrite();Check(!pass.Begin(f.profile,f.Call(),false),"opaque deferred writes invalidate jitter");
        f.Jitter(std::numeric_limits<float>::quiet_NaN(),0);Check(!pass.Begin(f.profile,f.Call(),false),"nonfinite jitter rejected");
        f.Jitter();f.profile.encoding=Encoding::SignedSquare;
        const float unrelated[4]{1,2,3,4};const D3D11_BOX box{0,0,0,16,1,1};
        constants::Updated(f.context.Get(),f.jitter.Get(),0,&box,unrelated,D3D11_COPY_DISCARD);
        Check(!pass.Begin(f.profile,f.Call(),false),"DISCARD partial updates invalidate untouched jitter");
        f.Jitter();
        Check(pass.Begin(f.profile,f.Call(),true),pass.Error().c_str());f.Original();
        Check(pass.Finish([&](const Frame& frame){const auto v=f.Read(frame.motion);Check(v[0]<0&&v[1]>0,"signed-square motion decoder executes");f.context->CopyResource(frame.output,f.sentinel.Get());return true;}),"packed decoder output");f.Drain(pass);
        Check(pass.Begin(f.profile,f.Call(),true),"abandoned capture begins");pass.Abandon();f.Drain(pass);
        // Ignore INFO/WARNING messages about the fixture's deliberately unused
        // bindings; ERROR/CORRUPTION from production capture/restore must be zero.
        for(UINT64 i=0;i<f.debug->GetNumStoredMessagesAllowedByRetrievalFilter();++i) {
            SIZE_T size=0;f.debug->GetMessage(i,nullptr,&size);std::vector<unsigned char> storage(size);
            auto* message=reinterpret_cast<D3D11_MESSAGE*>(storage.data());Hr(f.debug->GetMessage(i,message,&size));
            if(message->Severity<=D3D11_MESSAGE_SEVERITY_ERROR)std::fprintf(stderr,"D3D error: %s\n",message->pDescription);
            Check(message->Severity>D3D11_MESSAGE_SEVERITY_ERROR,"D3D11 debug validation");
        }
        std::printf("PASS TAA production profile/capture/decode/copyback/state/lifetime WARP: %u checks\n",checks);
    } catch(const std::exception& e) {std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
