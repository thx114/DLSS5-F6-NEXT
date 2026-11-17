#include <inputs/taa/TaaConsumerDx11.h>
#include <inputs/taa/TaaDeliveryPolicy.h>
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <cmath>
#include <d3d11sdklayers.h>
#include <d3d11_4.h>
#include <future>
#include <thread>
using namespace taa;
namespace {
unsigned checks=0;
void Check(bool ok,const char* label){++checks;if(!ok)throw std::runtime_error(label);}
void Hr(HRESULT result){Check(SUCCEEDED(result),"D3D11 API");}
ComPtr<ID3DBlob> Compile(const char* text,const char* target,UINT flags=0){ComPtr<ID3DBlob> code,errors;auto hr=D3DCompile(text,std::strlen(text),nullptr,nullptr,nullptr,"main",target,flags,0,&code,&errors);if(FAILED(hr)&&errors)std::fprintf(stderr,"%s",static_cast<char*>(errors->GetBufferPointer()));Hr(hr);return code;}
struct Fixture {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;ComPtr<ID3D11InfoQueue> debug;
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11DepthStencilState> noDepth;ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11Buffer> constants;
    Fixture(){Hr(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));Hr(device.As(&debug));
        auto v=Compile("struct O{float4 p:SV_Position;float2 uv:TEXCOORD;};O main(uint i:SV_VertexID){O o;o.uv=float2((i<<1)&2,i&2);o.p=float4(o.uv*float2(2,-2)+float2(-1,1),0,1);return o;}","vs_5_0");Hr(device->CreateVertexShader(v->GetBufferPointer(),v->GetBufferSize(),nullptr,&vs));
        auto p=Compile("Texture2D<float4> a:register(t0);Texture2D<float4> bloom:register(t1);SamplerState s:register(s0);cbuffer C:register(b0){float4 k;};float4 main(float4 p:SV_Position,float2 uv:TEXCOORD):SV_Target{return a.Sample(s,uv)*k+bloom.Sample(s,uv);}","ps_5_0");Hr(device->CreatePixelShader(p->GetBufferPointer(),p->GetBufferSize(),nullptr,&ps));
        D3D11_DEPTH_STENCIL_DESC depth{};Hr(device->CreateDepthStencilState(&depth,&noDepth));D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;Hr(device->CreateSamplerState(&sd,&sampler));
        D3D11_BUFFER_DESC bd{};bd.ByteWidth=16;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;Hr(device->CreateBuffer(&bd,nullptr,&constants));constants::Observe(constants.Get());SetConstant(.5f);
    }
    ComPtr<ID3D11Texture2D> Texture(unsigned size,DXGI_FORMAT format,float value,unsigned channels,UINT bind=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET|D3D11_BIND_UNORDERED_ACCESS){D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=size;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=format;d.BindFlags=bind;std::vector<float> values(size*size*channels,value);D3D11_SUBRESOURCE_DATA init{values.data(),size*channels*4,0};ComPtr<ID3D11Texture2D> out;Hr(device->CreateTexture2D(&d,&init,&out));return out;}
    ComPtr<ID3D11ShaderResourceView> Srv(ID3D11Texture2D* texture){ComPtr<ID3D11ShaderResourceView> out;Hr(device->CreateShaderResourceView(texture,nullptr,&out));return out;}
    ComPtr<ID3D11RenderTargetView> Rtv(ID3D11Texture2D* texture){ComPtr<ID3D11RenderTargetView> out;Hr(device->CreateRenderTargetView(texture,nullptr,&out));return out;}
    void SetConstant(float value){float v[]{value,value,value,value};context->UpdateSubresource(constants.Get(),0,nullptr,v,0,0);constants::Updated(context.Get(),constants.Get(),0,nullptr,v);}
    void Bind(ID3D11ShaderResourceView* source,ID3D11ShaderResourceView* bloom,ID3D11RenderTargetView* destination,unsigned size){context->ClearState();context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->OMSetRenderTargets(1,&destination,nullptr);context->OMSetDepthStencilState(noDepth.Get(),0);ID3D11ShaderResourceView* views[]{source,bloom};context->PSSetShaderResources(0,2,views);ID3D11SamplerState* sam=sampler.Get();context->PSSetSamplers(0,1,&sam);ID3D11Buffer* cb=constants.Get();context->PSSetConstantBuffers(0,1,&cb);D3D11_VIEWPORT vp{0,0,float(size),float(size),0,1};context->RSSetViewports(1,&vp);}
    float Pixel(ID3D11Texture2D* texture,unsigned x,unsigned y){D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);d.BindFlags=d.MiscFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Texture2D> staging;Hr(device->CreateTexture2D(&d,nullptr,&staging));context->CopyResource(staging.Get(),texture);D3D11_MAPPED_SUBRESOURCE mapped{};Hr(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));float v=*(reinterpret_cast<float*>(static_cast<unsigned char*>(mapped.pData)+y*mapped.RowPitch)+x*4);context->Unmap(staging.Get(),0);return v;}
};
}
int main(){try {
    const auto Demand=[](bool ok,const auto& source){++checks;if(!ok)throw std::runtime_error(source.Error());};
    consumer::DeliveryPolicy policy;
    Check(!policy.Defer(OutputRoute::ObserveUpscale,256,256),"unobserved route retains original-target SR");
    Check(policy.Observe(256,256,427,427),"actual larger consumer schedules later proof");
    Check(policy.Defer(OutputRoute::ObserveUpscale,256,256),"observed same-layout chain may defer for current proof");
    Check(!policy.Defer(OutputRoute::TaaTarget,256,256),"explicit original-target route ignores screen gap");
    Check(!policy.Defer(OutputRoute::ObserveUpscale,128,128),"changed extent cannot reuse scheduling hint");
    Check(policy.OutputWidth()==427&&policy.OutputHeight()==427,"the actual consumer owns persistent output dimensions");
    Check(policy.Observe(256,256,512,512)&&policy.Defer(OutputRoute::ObserveUpscale,256,256)&&policy.OutputWidth()==512,"a new physical extent replaces the stable layout before provider retirement");
    policy.Revoke();Check(!policy.Defer(OutputRoute::ObserveUpscale,256,256),"explicit configuration revocation forgets the learned extent");
    const auto declared=bindings::Read("vs_5_0\ndcl_constantbuffer CB0[16], dynamicIndexed\ndcl_constantbuffer CB13[4096], immediateIndexed\ndcl_resource_texture2d (float,float,float,float) t127\ndcl_sampler s15, mode_default\nret\n");
    Check(declared.valid&&declared.constants==std::vector<unsigned>({0,13})&&declared.constantBytes[0]==256&&declared.constantBytes[13]==65536&&declared.textures==std::vector<unsigned>({127})&&declared.samplers==std::vector<unsigned>({15}),"executable declarations retain fixed slots and complete CB extents");
    for(const auto* bad:{"dcl_constantbuffer CB14[1], immediateIndexed","dcl_constantbuffer CB0[4097], immediateIndexed","dcl_constantbuffer CB0[0], immediateIndexed","dcl_constantbuffer CB0[1], invalidIndexed","dcl_constantbuffer CB0[1], immediateIndexed\ndcl_constantbuffer CB0[1], immediateIndexed","dcl_resource_texture2d (float,float,float,float) t128","dcl_resource_texture2d (float,float,float,float,) t0","dcl_resource_texture2d (float,float,float,float) t0[2]","dcl_resource_texture2d (float,float,float,float) t0\ndcl_resource_texture2d (float,float,float,float) t0","dcl_sampler s16, mode_default","dcl_sampler s0, mode_comparison","dcl_sampler s0, mode_default\ndcl_sampler s0, mode_default","dcl_resource_structured t0, 4","dcl_resource_raw t0","dcl_resource_texture3d (float,float,float,float) t0","dcl_uav_typed_texture2d (float,float,float,float) u0","dcl_tgsm_raw g0, 16","dcl_interface fp0[1][1] = {ft0}","dcl_function_body fb0","dcl_function_table ft0 = {fb0}","interface_call fp0[0][0]","call l0","callc_nz r0.x, l0","ret\ndcl_constantbuffer CB0[1], immediateIndexed","dcl_unknown_stateful_resource t0"})
        Check(!bindings::Read(std::string("vs_5_0\n")+bad+"\nret\n").valid,"unsupported, duplicate, malformed and dynamic linkage declarations fail closed");
    {
        auto code=Compile("cbuffer C:register(b0){float k;}float4 main(float2 p:POSITION):SV_Position{return float4(p*k,0,1);}","vs_5_0");
        ComPtr<ID3DBlob> stripped;Hr(D3DStripShader(code->GetBufferPointer(),code->GetBufferSize(),D3DCOMPILER_STRIP_REFLECTION_DATA,&stripped));
        const auto program=consumer::Describe(stripped->GetBufferPointer(),stripped->GetBufferSize(),consumer::ProgramStage::Vertex);
        Check(program.supported&&program.constants==std::vector<unsigned>({0}),"stripped RDEF keeps executable vertex constants");
    }
    // Describe must prove the real position program, including new UV variants.
    const auto describeVertex=[](const std::string& body,UINT flags=0) {
        const std::string source="cbuffer C:register(b0){float k;} struct O{float4 p:SV_Position;float2 uv:TEXCOORD;};O main(float2 p:POSITION){O o;"+body+"return o;}";
        auto code=Compile(source.c_str(),"vs_5_0",flags);return consumer::Describe(code->GetBufferPointer(),code->GetBufferSize(),consumer::ProgramStage::Vertex);
    };
    const auto plain=describeVertex("o.p=float4(p,0,1);o.uv=p*float2(.5,-.5)+.5;");
    const auto uvFlip=describeVertex("o.p=float4(p,0,1);o.uv=p*.5+.5;");
    Check(plain.supported&&uvFlip.supported,"position proof accepts independent UV conventions without hash allowlists");
    const auto reflected=describeVertex("o.p=float4(p*float2(1,-1),0,1);o.uv=p*.5+.5;");
    fullscreen::Point reflectedPoint;const float probe[]{-1,1};
    Check(reflected.supported&&reflected.position.Apply(probe,reflectedPoint)&&reflectedPoint.x==-1&&reflectedPoint.y==-1,"position proof executes actual reflection for geometry");
    const auto partialProgram=describeVertex("o.p=float4(p*.5+.25,0,1);o.uv=p*.5+.5;");
    fullscreen::Point partialPoint;
    Check(partialProgram.supported&&partialProgram.position.Apply(probe,partialPoint)&&partialPoint.x==-.25&&partialPoint.y==.75,"literal transform preserves ordered float operations");
    const auto constantPosition=describeVertex("o.p=float4(p*k,0,1);o.uv=p;");
    Check(constantPosition.supported&&!constantPosition.position.Apply(probe,partialPoint),"parsed CB dependency cannot authorize geometry without current binding");
    const auto constantW=describeVertex("o.p=float4(p,0,k);o.uv=p;");
    Check(constantW.supported&&!constantW.position.Apply(probe,partialPoint),"homogeneous CB dependency remains unresolved before binding");
    const auto varyingDepth=describeVertex("o.p=float4(p,p.x,1);o.uv=p;");
    Check(varyingDepth.supported&&!varyingDepth.position.Apply(probe,partialPoint),"actual varying clip depth outside the clip range is refused");
    Check(!describeVertex("o.p=float4(p.x*p.x,p.y,0,1);o.uv=p;").supported,"nonlinear position stays unsupported");
    Check(!describeVertex("o.p=float4(p,0,1);if(k>0)o.p.x*=.5;o.uv=p;").supported,"conditional partial coverage stays unsupported");
    Check(!describeVertex("o.p=float4(p,-0.0000001,1);o.uv=p;").supported,"lossless immediate keeps negative near-zero clip depth invalid");
    Check(!describeVertex("o.p=float4(p,1.0000001,1);o.uv=p;").supported,"lossless immediate keeps above-one clip depth invalid");
    Check(!describeVertex("o.p=float4(p,0,1.0000001);o.uv=p;").supported,"lossless immediate keeps non-unit near-one homogeneous w invalid");
    const auto nanZero=describeVertex("float a=sqrt(k);float b=a*0.0;o.p=float4(p+b,0,1);o.uv=p;",D3DCOMPILE_SKIP_OPTIMIZATION);
    Check(!nanZero.supported,"NaN times zero must retain its unknown dependency");
    const auto cancellation=describeVertex("float2 a=p*100000000.0;float2 b=a+p;float2 c=b-a;o.p=float4(c,0,1);o.uv=p;",D3DCOMPILE_SKIP_OPTIMIZATION);
    Check(!cancellation.supported||!cancellation.position.Apply(probe,partialPoint),"finite large-small cancellation cannot authorize actual vertices");
    const auto overflow=describeVertex("float2 a=(p*1e20)*1e20;float2 c=a-a;o.p=float4(p+c,0,1);o.uv=p;",D3DCOMPILE_SKIP_OPTIMIZATION);
    Check(!overflow.supported||!overflow.position.Apply(probe,partialPoint),"overflow cancellation cannot be folded into identity");
    const auto hugeScale=describeVertex("o.p=float4(((p*1e20)*1e20)*0+p,0,1);o.uv=p;",D3DCOMPILE_SKIP_OPTIMIZATION);
    Check(!hugeScale.supported||!hugeScale.position.Apply(probe,partialPoint),"zero multiplication cannot erase overflow before coverage proof");
    const auto overflowingAffine=describeVertex("o.p=float4((p*1e20)*1e20,0,1);o.uv=p;",D3DCOMPILE_SKIP_OPTIMIZATION);
    fullscreen::Point overflowPoint;
    Check(!overflowingAffine.supported||!overflowingAffine.position.Apply(probe,overflowPoint),"overflowing affine program cannot authorize actual vertices");
    auto structured=Compile("StructuredBuffer<float> sb:register(t0);float4 main(float2 p:POSITION):SV_Position{return float4(p,sb[0],1);}","vs_5_0");
    const auto rejectedResource=consumer::Describe(structured->GetBufferPointer(),structured->GetBufferSize(),consumer::ProgramStage::Vertex);
    Check(!rejectedResource.supported&&rejectedResource.hash==DrawObservationDx11::ShaderByteHash(structured->GetBufferPointer(),structured->GetBufferSize()),"semantic refusal retains actual byte identity");
    Fixture f;
    {
        ID3D11Buffer* raw=f.constants.Get();f.context->VSSetConstantBuffers(0,1,&raw);
        auto scale=constantPosition.position;Check(scale.BindConstants(f.context.Get())&&scale.Apply(probe,partialPoint)&&partialPoint.x==-.5&&partialPoint.y==.5,"actual known VS constants bind without guessing");
        auto homogeneous=constantW.position;Check(homogeneous.BindConstants(f.context.Get())&&!homogeneous.Apply(probe,partialPoint),"actual non-unit CB homogeneous w refuses geometry");
        f.SetConstant(1);homogeneous=constantW.position;Check(homogeneous.BindConstants(f.context.Get())&&homogeneous.Apply(probe,partialPoint),"fresh unit homogeneous binding may pass");
        auto unknown=constantPosition.position;constants::InvalidateConstants();
        Check(!unknown.BindConstants(f.context.Get()),"unknown CB bytes cannot authorize position");
        f.SetConstant(std::nanf(""));auto invalid=constantPosition.position;Check(!invalid.BindConstants(f.context.Get()),"NaN CB lane refuses binding");
        f.SetConstant(.5f);ComPtr<ID3D11DeviceContext1> c1;Hr(f.context.As(&c1));
        float values[128]{};values[64]=values[65]=values[66]=values[67]=1;
        D3D11_BUFFER_DESC d{};d.ByteWidth=sizeof(values);d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;D3D11_SUBRESOURCE_DATA init{values,0,0};ComPtr<ID3D11Buffer> ranged;
        Hr(f.device->CreateBuffer(&d,&init,&ranged));constants::Initialized(ranged.Get(),&init);raw=ranged.Get();UINT first=16,count=16;c1->VSSetConstantBuffers1(0,1,&raw,&first,&count);
        auto bound=constantPosition.position;Check(bound.BindConstants(f.context.Get())&&bound.Apply(probe,partialPoint)&&partialPoint.x==-1&&partialPoint.y==1,"actual VS CB1 first-constant range is honored");
        raw=nullptr;f.context->VSSetConstantBuffers(0,1,&raw);auto absent=constantPosition.position;Check(!absent.BindConstants(f.context.Get()),"missing VS CB never uses an earlier binding");
    }
    {
        auto code=Compile("struct O{float4 p:SV_Position;float2 uv:TEXCOORD;};O main(float2 p:POSITION){O o;o.p=float4(p,0,1);o.uv=p*float2(.5,-.5)+.5;return o;}","vs_5_0");
        ComPtr<ID3D11InputLayout> layout;const D3D11_INPUT_ELEMENT_DESC element{"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0};
        Hr(f.device->CreateInputLayout(&element,1,code->GetBufferPointer(),code->GetBufferSize(),&layout));fullscreen::Remember(layout.Get(),&element,1);f.context->IASetInputLayout(layout.Get());
        float vertices[]{-1,1,3,1,-1,-3};D3D11_BUFFER_DESC desc{};desc.ByteWidth=sizeof(vertices);desc.BindFlags=D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA data{vertices,0,0};ComPtr<ID3D11Buffer> buffer;Hr(f.device->CreateBuffer(&desc,&data,&buffer));constants::Initialized(buffer.Get(),&data);
        ID3D11Buffer* raw=buffer.Get();UINT stride=8,offset=0;f.context->IASetVertexBuffers(0,1,&raw,&stride,&offset);f.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        Check(fullscreen::Geometry(f.context.Get(),false,3,0,0),"real observed clip-space triangle covers full viewport");
        Check(fullscreen::Geometry(f.context.Get(),false,3,0,0,nullptr,&plain.position),"ordered identity program covers the actual vertices");
        Check(!fullscreen::Geometry(f.context.Get(),false,3,0,0,nullptr,&partialProgram.position),"raw fullscreen vertices cannot authorize transformed partial coverage");
        Check(!fullscreen::Geometry(f.context.Get(),false,3,0,0,nullptr,&overflowingAffine.position),"actual transformed geometry refuses overflowing instructions");
        auto expanded=describeVertex("o.p=float4(p*2,0,1);o.uv=p+.5;");
        float halfVertices[]{-.5f,.5f,1.5f,.5f,-.5f,-1.5f};
        constants::Updated(f.context.Get(),buffer.Get(),0,nullptr,halfVertices);
        Check(expanded.supported&&fullscreen::Geometry(f.context.Get(),false,3,0,0,nullptr,&expanded.position),"literal position scale proves coverage from actual smaller vertices");
        constants::Updated(f.context.Get(),buffer.Get(),0,nullptr,vertices);
        constants::InvalidateConstants();Check(fullscreen::Geometry(f.context.Get(),false,3,0,0),"SR configuration does not erase unchanged geometry provenance");
        ComPtr<ID3D11Device> auxiliary;ComPtr<ID3D11DeviceContext> auxiliaryContext;Hr(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&auxiliary,nullptr,&auxiliaryContext));
        constants::OpaqueWrite(auxiliaryContext.Get());Check(fullscreen::Geometry(f.context.Get(),false,3,0,0),"auxiliary device cannot revoke the source device geometry");
        constants::OpaqueWrite(f.context.Get());Check(!fullscreen::Geometry(f.context.Get(),false,3,0,0),"same-device opaque write still revokes geometry");constants::Initialized(buffer.Get(),&data);
        vertices[2]=0;vertices[5]=0;constants::Updated(f.context.Get(),buffer.Get(),0,nullptr,vertices);Check(!fullscreen::Geometry(f.context.Get(),false,3,0,0),"full viewport with small triangle is not full coverage");
        constants::Written(buffer.Get());Check(!fullscreen::Geometry(f.context.Get(),false,3,0,0),"opaque vertex writes cannot reuse old geometry");
        vertices[2]=3;vertices[5]=-3;
        ComPtr<ID3D11Buffer> recovered;Hr(f.device->CreateBuffer(&desc,&data,&recovered));constants::Initialized(recovered.Get(),&data);
        raw=recovered.Get();f.context->IASetVertexBuffers(0,1,&raw,&stride,&offset);constants::OpaqueWrite(f.context.Get());
        fullscreen::Evidence evidence(f.context.Get());
        const auto finish=[&]{f.context->Flush();const auto deadline=GetTickCount64()+5000;while(!evidence.Idle()){if(GetTickCount64()>=deadline)throw std::runtime_error("geometry EVENT retirement deadline");SwitchToThread();}};
        Check(!evidence.Recover(recovered.Get(),false),"revoked static geometry queues a current asynchronous copy");finish();
        Check(evidence.Recover(recovered.Get(),false)&&fullscreen::Geometry(f.context.Get(),false,3,0,0),"completed current GPU bytes restore untouched DEFAULT geometry");
        constants::ReadbackStamp oldStamp;Check(constants::StampReadback(recovered.Get(),oldStamp),"static source stamp before opaque submission");
        {
            constants::GeometrySubmission submission(f.context.Get());constants::ReadbackStamp blocked;
            Check(!constants::StampReadback(recovered.Get(),blocked),"active command-list submission rejects new recovery stamps");
            Check(!constants::PublishReadback(recovered.Get(),oldStamp,vertices,sizeof(vertices)),"active command-list submission rejects an older completed copy");
            Check(!fullscreen::Geometry(f.context.Get(),false,3,0,0),"active opaque submission revokes recovered Snapshot authority");
            {constants::GeometrySubmission nested(f.context.Get());Check(!constants::StampReadback(recovered.Get(),blocked),"recursive opaque submissions retain the domain boundary");}
            Check(!constants::StampReadback(recovered.Get(),blocked),"nested scope exit cannot reopen its outer submission");
        }
        Check(!constants::PublishReadback(recovered.Get(),oldStamp,vertices,sizeof(vertices)),"completed opaque submission cannot publish a pre-scope copy");
        Check(!evidence.Recover(recovered.Get(),false),"closed current domain schedules its own copy");finish();
        Check(evidence.Recover(recovered.Get(),false)&&fullscreen::Geometry(f.context.Get(),false,3,0,0),"untouched static source recovers in the new domain");
        constants::OpaqueWrite(f.context.Get());Check(!evidence.Recover(recovered.Get(),false),"prepare a copy before a concurrent target writer");
        ComPtr<ID3D11Multithread> threading;Hr(f.context.As(&threading));threading->SetMultithreadProtected(TRUE);
        std::promise<void> entered,submit;auto enteredFuture=entered.get_future();auto submitFuture=submit.get_future();
        vertices[2]=0;vertices[5]=0;
        std::thread writer([&]{constants::GeometryWrite(recovered.Get());entered.set_value();submitFuture.wait();f.context->UpdateSubresource(recovered.Get(),0,nullptr,vertices,0,0);constants::Updated(f.context.Get(),recovered.Get(),0,nullptr,vertices);});
        enteredFuture.wait();constants::ReadbackStamp interleaved;
        const bool rejectedBeforeSubmit=!constants::StampReadback(recovered.Get(),interleaved);
        submit.set_value();writer.join();Check(rejectedBeforeSubmit,"pre-driver writer mark blocks owner recovery before actual write submission");finish();
        Check(!evidence.Recover(recovered.Get(),false),"completed old GPU copy cannot regain authority after targeted writing");
        Check(!fullscreen::Geometry(f.context.Get(),false,3,0,0),"CPU Updated cannot reopen a buffer admitted as static recovery");
        Check(!evidence.Recover(recovered.Get(),false),"targeted writes permanently disable this static recovery mode");
        Check(evidence.Idle(),"all geometry copies retire through their own completed EVENT");

    }
    consumer::Program pixel{true,{0},{0,1}},vertex{true,{},{}};
    {
        auto root=f.Texture(8,DXGI_FORMAT_R32G32B32A32_FLOAT,.33f,4),low=f.Texture(8,DXGI_FORMAT_R32G32B32A32_FLOAT,0,4),high=f.Texture(24,DXGI_FORMAT_R32G32B32A32_FLOAT,0,4);
        auto rootView=f.Srv(root.Get()),lowView=f.Srv(low.Get());auto lowRt=f.Rtv(low.Get()),highRt=f.Rtv(high.Get());
        consumer::Chain observation(f.context.Get());observation.Reset(root.Get(),true);f.SetConstant(1);
        f.Bind(rootView.Get(),nullptr,lowRt.Get(),8);Demand(observation.Before({false,3},pixel,vertex,8,8),observation);
        Check(!observation.NeedsResolve(),"same-size observation only publishes lineage");f.context->Draw(3,0);Check(!observation.Returned(),"observation cannot claim delivery");
        f.Bind(lowView.Get(),nullptr,highRt.Get(),24);Demand(observation.Before({false,3},pixel,vertex,8,8)&&observation.NeedsResolve(),observation);
        f.context->Draw(3,0);Check(!observation.Returned(),"physical route observation preserves original draw");
        Check(observation.Activity().draws==2&&observation.Activity().linked==2,"bounded linked draw counters retain topology");
        Check(std::abs(f.Pixel(root.Get(),1,1)-.33f)<.00001f,"observation leaves original-target output intact");
        observation.Reset(root.Get(),true);f.Bind(rootView.Get(),nullptr,highRt.Get(),24);
        Check(!observation.Before({false,3,0,1,1},pixel,vertex,8,8)&&observation.Activity().reason=="unsupported compositor command","nonzero instance origin cannot be replayed as a non-instanced command");
        observation.Reset(root.Get(),true);f.Bind(rootView.Get(),nullptr,highRt.Get(),24);consumer::Program unknown;
        Check(!observation.Before({false,3},unknown,vertex,8,8)&&observation.Activity().reason=="linked compositor pixel program is unsupported","unsupported linked program has explicit reason");
        observation.Reset(root.Get(),true);f.Bind(rootView.Get(),rootView.Get(),lowRt.Get(),8);
        Check(!observation.Before({false,3},pixel,vertex,8,8)&&observation.Activity().reason=="compositor secondary input aliases the TAA chain","same-size secondary root alias is not replayable");
        observation.Reset(root.Get(),true);f.Bind(rootView.Get(),nullptr,lowRt.Get(),8);
        Demand(observation.Before({false,3},pixel,vertex,8,8),observation);f.context->Draw(3,0);observation.Returned();
        f.Bind(lowView.Get(),rootView.Get(),highRt.Get(),24);
        Check(!observation.Before({false,3},pixel,vertex,8,8)&&!observation.NeedsResolve()&&observation.Activity().reason=="compositor secondary input aliases the TAA chain","final edge cannot mix a reconstructed input with the old root");
    }
    for(unsigned mode=0;mode<4;++mode){
        auto source=f.Texture(8,DXGI_FORMAT_R32G32B32A32_FLOAT,.1f,4);auto depth=f.Texture(8,DXGI_FORMAT_R32_FLOAT,.75f,1);auto motion=f.Texture(8,DXGI_FORMAT_R32G32_FLOAT,0,2);
        auto original=f.Texture(8,DXGI_FORMAT_R32G32B32A32_FLOAT,.33f,4);auto intermediate=f.Texture(8,DXGI_FORMAT_R32G32B32A32_FLOAT,0,4);auto high=f.Texture(24,DXGI_FORMAT_R32G32B32A32_FLOAT,0,4);auto bloom=f.Texture(8,DXGI_FORMAT_R32G32B32A32_FLOAT,.2f,4);
        auto sourceView=f.Srv(source.Get()),depthView=f.Srv(depth.Get()),motionView=f.Srv(motion.Get()),originalView=f.Srv(original.Get()),intermediateView=f.Srv(intermediate.Get()),bloomView=f.Srv(bloom.Get());
        auto originalRt=f.Rtv(original.Get()),intermediateRt=f.Rtv(intermediate.Get()),highRt=f.Rtv(high.Get());
        f.Bind(sourceView.Get(),bloomView.Get(),originalRt.Get(),8);f.SetConstant(0);
        ID3D11ShaderResourceView* inputs[]{depthView.Get(),motionView.Get(),sourceView.Get()};f.context->PSSetShaderResources(0,3,inputs);
        Profile profile;profile.colorSlot=2;profile.depthSlot=0;profile.motionSlot=1;profile.jitterSlot=0;profile.jitterOffsetX=0;profile.jitterOffsetY=4;profile.jitterScaleX=profile.jitterScaleY=profile.motionScaleX=profile.motionScaleY=1;
        PassDx11 pass(f.device.Get(),f.context.Get());Demand(pass.Begin(profile,{Stage::Pixel,3},true),pass);
        // Original TAA/history stays low resolution and is never overwritten by SR.
        consumer::Chain chain(f.context.Get());chain.Reset(original.Get());
        f.SetConstant(.5f);
        if(mode!=0){f.Bind(originalView.Get(),bloomView.Get(),intermediateRt.Get(),8);Demand(chain.Before({false,3},pixel,vertex,8,8),chain);Check(!chain.NeedsResolve(),"same-size postprocess is frozen, not evaluated");f.context->Draw(3,0);Check(!chain.Returned(),"low postprocess only publishes lineage");
            f.SetConstant(.9f);const float changed[]{.8f,.8f,.8f,.8f};f.context->ClearRenderTargetView(f.Rtv(bloom.Get()).Get(),changed);
        }
        f.Bind(mode?intermediateView.Get():originalView.Get(),bloomView.Get(),highRt.Get(),24);
        if(mode==2){chain.Invalidate(intermediate.Get());Check(!chain.Before({false,3},pixel,vertex,8,8),"unknown overwrite revokes exact edge");pass.Abandon();continue;}
        if(mode==3){D3D11_VIEWPORT partial{0,0,12,24,0,1};f.context->RSSetViewports(1,&partial);Check(!chain.Before({false,3},pixel,vertex,8,8),"partial viewport cannot authorize full-size resolve");pass.Abandon();continue;}
        Demand(chain.Before({false,3},pixel,vertex,8,8)&&chain.NeedsResolve(),chain);
        Check(pass.Resolve(chain.Width(),chain.Height(),[&](const Frame& frame){Check(frame.renderWidth==8&&frame.outputWidth==24&&frame.outputHeight==24,"SR provider sees actual low-to-high extents");std::vector<float> data(24*24*4);for(unsigned y=0;y<24;++y)for(unsigned x=0;x<24;++x)for(unsigned c=0;c<4;++c)data[(y*24+x)*4+c]=float(x)/24;f.context->UpdateSubresource(frame.output,0,nullptr,data.data(),24*16,0);return true;},false),"full-size Resolve failed");
        Check(!pass.Idle(),"freeze slot cannot retire before physical consumer");Demand(chain.Bind(pass.CurrentOutput()),chain);
        f.context->Draw(3,0);Check(chain.Returned(),"only physical consumer return marks delivery");
        ComPtr<ID3D11ShaderResourceView> restored;f.context->PSGetShaderResources(0,1,&restored);Check(restored.Get()==(mode?intermediateView.Get():originalView.Get()),"consumer SRV restored exactly");pass.Abandon();
        const float expected=mode?(.5f*.5f+.2f)*.9f+.8f:.5f*.5f+.2f;
        Check(std::abs(f.Pixel(high.Get(),12,12)-expected)<.00001f,"full-size sidecar preserves frozen tone/bloom/constant order");Check(std::abs(f.Pixel(original.Get(),1,1)-.33f)<.00001f,"original low TAA/history preserved");
        f.context->Flush();for(unsigned tries=0;!pass.Idle()&&tries<2000;++tries)Sleep(1);Check(pass.Idle(),"consumer-tail EVENT retires slot");
    }
    {
        // Every edge retains its own typed SRV. Decode of the first node's
        // UNORM bytes by the second node's sRGB view must survive replay.
        const auto typed=[&](unsigned size,DXGI_FORMAT format){D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=size;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=format;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;ComPtr<ID3D11Texture2D> t;Hr(f.device->CreateTexture2D(&d,nullptr,&t));return t;};
        const auto srv=[&](ID3D11Texture2D* t,DXGI_FORMAT format){D3D11_SHADER_RESOURCE_VIEW_DESC d{};d.Format=format;d.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;d.Texture2D.MipLevels=1;ComPtr<ID3D11ShaderResourceView> v;Hr(f.device->CreateShaderResourceView(t,&d,&v));return v;};
        const auto rtv=[&](ID3D11Texture2D* t,DXGI_FORMAT format){D3D11_RENDER_TARGET_VIEW_DESC d{};d.Format=format;d.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;ComPtr<ID3D11RenderTargetView> v;Hr(f.device->CreateRenderTargetView(t,&d,&v));return v;};
        auto root=typed(8,DXGI_FORMAT_R8G8B8A8_TYPELESS),a=typed(8,DXGI_FORMAT_R8G8B8A8_TYPELESS),b=typed(8,DXGI_FORMAT_R8G8B8A8_TYPELESS),resolved=typed(24,DXGI_FORMAT_R8G8B8A8_UNORM);
        auto rootView=srv(root.Get(),DXGI_FORMAT_R8G8B8A8_UNORM_SRGB),aView=srv(a.Get(),DXGI_FORMAT_R8G8B8A8_UNORM_SRGB),bView=srv(b.Get(),DXGI_FORMAT_R8G8B8A8_UNORM);
        auto rootRt=rtv(root.Get(),DXGI_FORMAT_R8G8B8A8_UNORM),aRt=rtv(a.Get(),DXGI_FORMAT_R8G8B8A8_UNORM),bRt=rtv(b.Get(),DXGI_FORMAT_R8G8B8A8_UNORM_SRGB),resolvedRt=rtv(resolved.Get(),DXGI_FORMAT_R8G8B8A8_UNORM);
        const float initial[]{.5f,.5f,.5f,1};f.context->ClearRenderTargetView(rootRt.Get(),initial);f.context->ClearRenderTargetView(resolvedRt.Get(),initial);
        auto black=f.Texture(8,DXGI_FORMAT_R32G32B32A32_FLOAT,0,4);auto blackView=f.Srv(black.Get());
        auto reference=f.Texture(24,DXGI_FORMAT_R32G32B32A32_FLOAT,0,4),actual=f.Texture(24,DXGI_FORMAT_R32G32B32A32_FLOAT,0,4);auto referenceRt=f.Rtv(reference.Get()),actualRt=f.Rtv(actual.Get());
        consumer::Chain chain(f.context.Get());chain.Reset(root.Get());f.SetConstant(.5f);
        f.Bind(rootView.Get(),blackView.Get(),aRt.Get(),8);Demand(chain.Before({false,3},pixel,vertex,8,8),chain);f.context->Draw(3,0);chain.Returned();
        f.Bind(aView.Get(),blackView.Get(),bRt.Get(),8);Demand(chain.Before({false,3},pixel,vertex,8,8),chain);f.context->Draw(3,0);chain.Returned();
        f.SetConstant(1);f.Bind(bView.Get(),blackView.Get(),referenceRt.Get(),24);f.context->Draw(3,0);
        f.Bind(bView.Get(),blackView.Get(),actualRt.Get(),24);Demand(chain.Before({false,3},pixel,vertex,8,8)&&chain.NeedsResolve(),chain);Check(chain.ReplayCount()==2,"two typed-view nodes must replay in order");Demand(chain.Bind(resolved.Get()),chain);f.context->Draw(3,0);Check(chain.Returned(),"typed chain physical delivery");
        const float expected=f.Pixel(reference.Get(),12,12);Check(expected>.04f&&expected<.1f,"sRGB reference exercises decode and encode");Check(std::abs(f.Pixel(actual.Get(),12,12)-expected)<.00001f,"per-node sRGB view semantics survive full-size replay");
    }
    {
        consumer::Surface cached;Check(consumer::MakeSurface(f.device.Get(),24,24,DXGI_FORMAT_R8G8B8A8_UNORM,cached),"UNORM surface allocation");
        Check(consumer::MakeSurface(f.device.Get(),24,24,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,cached),"changed typed surface allocation");
        D3D11_RENDER_TARGET_VIEW_DESC r{};cached.rtv->GetDesc(&r);Check(r.Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,"cached node output retains this frame's typed RTV");
    }
    {
        auto root=f.Texture(8,DXGI_FORMAT_R32G32B32A32_FLOAT,.1f,4),other=f.Texture(8,DXGI_FORMAT_R32G32B32A32_FLOAT,0,4),high=f.Texture(24,DXGI_FORMAT_R32G32B32A32_FLOAT,0,4);
        auto rootView=f.Srv(root.Get());auto otherRt=f.Rtv(other.Get()),highRt=f.Rtv(high.Get());
        ComPtr<ID3D11UnorderedAccessView> write;Hr(f.device->CreateUnorderedAccessView(root.Get(),nullptr,&write));consumer::Chain chain(f.context.Get());chain.Reset(root.Get());
        f.Bind(nullptr,nullptr,otherRt.Get(),8);ID3D11RenderTargetView* rt=otherRt.Get();ID3D11UnorderedAccessView* uav=write.Get();f.context->OMSetRenderTargetsAndUnorderedAccessViews(1,&rt,nullptr,1,1,&uav,nullptr);
        Check(!chain.Before({false,3},{},vertex,8,8),"unknown UAV writer is observation only");chain.Returned();
        f.Bind(rootView.Get(),nullptr,highRt.Get(),24);Check(!chain.Before({false,3},pixel,vertex,8,8),"unknown OM UAV write revokes tracked root before later consumer");
    }
    for(UINT64 i=0;i<f.debug->GetNumStoredMessages();++i){SIZE_T size=0;f.debug->GetMessage(i,nullptr,&size);std::vector<unsigned char> storage(size);auto* message=reinterpret_cast<D3D11_MESSAGE*>(storage.data());Hr(f.debug->GetMessage(i,message,&size));Check(message->Severity>D3D11_MESSAGE_SEVERITY_WARNING,message->pDescription);}
    std::printf("PASS TAA full-size consumer/replay/frozen inputs/retirement: %u checks\n",checks);
}catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
