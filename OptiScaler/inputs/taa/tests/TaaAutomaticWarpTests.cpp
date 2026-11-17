#include <inputs/taa/TaaAutoDx11.h>
#include <inputs/taa/TaaTemporalEvidenceDx11.h>
#include <inputs/taa/TaaPassDx11.h>
#include <inputs/taa/TaaSelection.h>
#include <cstdio>
#include <stdexcept>

using namespace taa;
namespace {
unsigned checks=0;
void Check(bool ok,const char* text){++checks;if(!ok)throw std::runtime_error(text);}
void Hr(HRESULT hr){Check(SUCCEEDED(hr),"WARP fixture API");}
const char* Source=R"(
Texture2D<float> sceneZ:register(t5);Texture2D<float2> flow:register(t7);
Texture2D<float4> image:register(t9),oldImage:register(t11);SamplerState linearSampler:register(s0);
cbuffer View:register(b3){float2 inverseSize;float2 offset;}
float4 main(float4 p:SV_Position):SV_Target0 {
 float2 uv=p.xy*inverseSize;float z=sceneZ.SampleLevel(linearSampler,uv,0);
 float other=sceneZ.SampleLevel(linearSampler,uv+inverseSize,0);
 float2 velocity=flow.SampleLevel(linearSampler,uv+float2(other<z?inverseSize.x:0,0),0);
 float4 current=image.SampleLevel(linearSampler,uv-offset,0),history=oldImage.SampleLevel(linearSampler,uv-velocity,0);
 return lerp(current,history,.5)+min(z,other)*.00001;
})";
analysis::Result Analyze(std::string source) {
    ComPtr<ID3DBlob> code,errors;Hr(D3DCompile(source.data(),source.size(),"automatic TAA WARP",nullptr,nullptr,"main","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors));
    return analysis::Analyze(code->GetBufferPointer(),code->GetBufferSize(),Stage::Pixel,42);
}
}
int main() {
    try {
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;ComPtr<ID3D11DeviceContext1> context1;
        Hr(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));Hr(context.As(&context1));
        const auto texture=[&](DXGI_FORMAT format,UINT flags){D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=16;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=format;d.BindFlags=flags;ComPtr<ID3D11Texture2D> t;Hr(device->CreateTexture2D(&d,nullptr,&t));return t;};
        auto depth=texture(DXGI_FORMAT_R32_TYPELESS,D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE);
        auto scalar=texture(DXGI_FORMAT_R32_FLOAT,D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE);
        auto motion=texture(DXGI_FORMAT_R16G16_FLOAT,D3D11_BIND_SHADER_RESOURCE);
        auto color=texture(DXGI_FORMAT_R16G16B16A16_FLOAT,D3D11_BIND_SHADER_RESOURCE);
        auto history=texture(DXGI_FORMAT_R16G16B16A16_FLOAT,D3D11_BIND_SHADER_RESOURCE);
        auto target=texture(DXGI_FORMAT_R16G16B16A16_FLOAT,D3D11_BIND_RENDER_TARGET|D3D11_BIND_UNORDERED_ACCESS);
        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=DXGI_FORMAT_R32_FLOAT;srv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;srv.Texture2D.MipLevels=1;
        ComPtr<ID3D11ShaderResourceView> depthView,scalarView,motionView,colorView,historyView;
        Hr(device->CreateShaderResourceView(depth.Get(),&srv,&depthView));Hr(device->CreateShaderResourceView(scalar.Get(),&srv,&scalarView));
        Hr(device->CreateShaderResourceView(motion.Get(),nullptr,&motionView));Hr(device->CreateShaderResourceView(color.Get(),nullptr,&colorView));Hr(device->CreateShaderResourceView(history.Get(),nullptr,&historyView));
        ComPtr<ID3D11DepthStencilView> dsv;D3D11_DEPTH_STENCIL_VIEW_DESC dd{};dd.Format=DXGI_FORMAT_D32_FLOAT;dd.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;Hr(device->CreateDepthStencilView(depth.Get(),&dd,&dsv));
        const auto bind=[&](unsigned slot,ID3D11ShaderResourceView* view){context->PSSetShaderResources(slot,1,&view);};
        bind(5,depthView.Get());bind(7,motionView.Get());bind(9,colorView.Get());bind(11,historyView.Get());
        ComPtr<ID3D11Buffer> cb;D3D11_BUFFER_DESC bd{};bd.ByteWidth=512;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;bd.Usage=D3D11_USAGE_DYNAMIC;bd.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;Hr(device->CreateBuffer(&bd,nullptr,&cb));
        ID3D11Buffer* bound=cb.Get();UINT first=16,count=16;context1->PSSetConstantBuffers1(3,1,&bound,&first,&count);
        context->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,1,0);automatic::DepthCleared(context.Get(),dsv.Get(),D3D11_CLEAR_DEPTH,1);
        const auto analyzed=Analyze(Source);
        auto resolved=automatic::Resolve(context.Get(),Stage::Pixel,analyzed);
        Check(!resolved.profile,"automatic capture waits for a real observed jitter write");
        const auto write=[&](float x,float y){D3D11_MAPPED_SUBRESOURCE mapped{};const auto hr=context->Map(cb.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped);Hr(hr);constants::Mapped(context.Get(),cb.Get(),0,D3D11_MAP_WRITE_DISCARD,&mapped,hr);std::memset(mapped.pData,0,512);const float values[]{1.0f/16,1.0f/16,x,y};std::memcpy(static_cast<unsigned char*>(mapped.pData)+256,values,sizeof(values));constants::BeforeUnmap(context.Get(),cb.Get(),0);context->Unmap(cb.Get(),0);constants::AfterUnmap(context.Get(),cb.Get(),0);};
        write(.25f/16,-.125f/16);
        evidence::Frame observed;observed.Constants(context.Get(),evidence::ShaderStage::Pixel,analyzed);observed.Bindings(context.Get(),Stage::Pixel);
        Check(observed.buffers.size()==32&&std::all_of(observed.buffers.begin()+16,observed.buffers.end(),[](auto v){return v==1;}),"companion evidence uses the actual CB1 slice and observed-byte mask");
        float capturedJitter=0;std::memcpy(&capturedJitter,observed.buffers.data()+8,4);
        Check(capturedJitter==.25f/16&&observed.metadata.find("first_byte=256")!=std::string::npos,"companion constants preserve current bound byte offsets and values");
        Check(evidence::ResourceId(depth.Get())!=0&&evidence::ResourceId(depth.Get())==evidence::ResourceId(depth.Get())&&evidence::ResourceId(depth.Get())!=evidence::ResourceId(motion.Get()),"diagnostic identities follow resource objects and distinguish same-extent inputs");
        Check(evidence::Targets(observed,analyzed,{Stage::Pixel,42},3).size()==3,"raster targets require the actual temporal color motion and unique native depth bindings");
        resolved=automatic::Resolve(context.Get(),Stage::Pixel,analyzed);
        Check(bool(resolved.profile),"real bound views and observed CB complete the automatic contract");
        Check(resolved.profile->colorSlot==9&&resolved.profile->depthSlot==5&&resolved.profile->motionSlot==7,"typed views eliminate scalar-as-current-color false candidates");
        Check(!resolved.profile->depthInverted&&resolved.profile->jitterOffsetX==8&&resolved.profile->jitterOffsetY==12,"native depth convention and CB-relative jitter offsets");
        ComPtr<ID3D11RenderTargetView> rtv;Hr(device->CreateRenderTargetView(target.Get(),nullptr,&rtv));ID3D11RenderTargetView* rt=rtv.Get();context->OMSetRenderTargets(1,&rt,nullptr);
        ComPtr<ID3D11DepthStencilState> noDepth;D3D11_DEPTH_STENCIL_DESC ds{};ds.DepthFunc=D3D11_COMPARISON_ALWAYS;Hr(device->CreateDepthStencilState(&ds,&noDepth));context->OMSetDepthStencilState(noDepth.Get(),0);
        D3D11_VIEWPORT vp{0,0,16,16,0,1};context->RSSetViewports(1,&vp);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        D3D11_RECT scissor{1,2,14,15};context->RSSetScissorRects(1,&scissor);
        D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sd.AddressU=D3D11_TEXTURE_ADDRESS_CLAMP;sd.AddressV=D3D11_TEXTURE_ADDRESS_WRAP;sd.AddressW=D3D11_TEXTURE_ADDRESS_BORDER;sd.MaxLOD=3;sd.MaxAnisotropy=1;
        ComPtr<ID3D11SamplerState> sampler;Hr(device->CreateSamplerState(&sd,&sampler));ID3D11SamplerState* rawSampler=sampler.Get();context->PSSetSamplers(0,1,&rawSampler);
        evidence::Frame diagnostic;diagnostic.Bindings(context.Get(),Stage::Pixel);
        Check(diagnostic.metadata.find("viewport0 x=0 y=0 width=16 height=16")!=std::string::npos&&diagnostic.metadata.find("scissor0 left=1 top=2 right=14 bottom=15")!=std::string::npos,"same-draw diagnostics preserve viewport and scissors");
        Check(diagnostic.metadata.find("sampler0 bound=true filter=21 address_u=3 address_v=1 address_w=4")!=std::string::npos&&diagnostic.metadata.find("max_lod=3")!=std::string::npos,"same-draw diagnostics retain sampler filtering addressing and LOD");
        Check(diagnostic.metadata.find("blend0 enabled=false")!=std::string::npos&&diagnostic.metadata.find("write_mask=15")!=std::string::npos&&diagnostic.metadata.find("raster bound=false fill=3 cull=3")!=std::string::npos,"null D3D11 raster and blend states are decoded as documented defaults");
        auto observeOnly=analyzed;observeOnly.recipes.clear();observeOnly.observation=analysis::ObservationContract{"fixture",{{5,"depth_hint"},{7,"motion_hint"},{9,"color_hint"}}};
        const auto diagnosticTargets=evidence::Targets(diagnostic,observeOnly,{Stage::Pixel,42},3);
        Check(diagnosticTargets.size()==3&&std::string_view(diagnosticTargets[1].role)=="motion_hint","unresolved observation roles can watch real producer objects without recipes");
        Check(!automatic::Resolve(context.Get(),Stage::Pixel,observeOnly).profile,"diagnostic bindings and roles cannot create replacement authority");
        PassDx11 probe(device.Get(),context.Get());Check(probe.Probe(*resolved.profile,{Stage::Pixel,3}),"automatic profile passes the production transaction's exact input checks");
        auto missing=std::string(Source);missing.replace(missing.find("uv-offset"),9,"uv");
        Check(!automatic::Resolve(context.Get(),Stage::Pixel,Analyze(missing)).profile,"no jitter relation cannot authorize replacement even with plausible textures");
        bind(5,scalarView.Get());Check(!automatic::Resolve(context.Get(),Stage::Pixel,analyzed).profile,"a scalar color attachment cannot masquerade as native device depth");bind(5,depthView.Get());
        context->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,0,0);automatic::DepthCleared(context.Get(),dsv.Get(),D3D11_CLEAR_DEPTH,0);
        Check(automatic::Resolve(context.Get(),Stage::Pixel,analyzed).profile->depthInverted,"depth evidence belongs to the actual bound native depth object");
        constants::OpaqueWrite();Check(!automatic::Resolve(context.Get(),Stage::Pixel,analyzed).profile,"opaque writes cannot replay an old jitter value");
        evidence::Frame unknown;unknown.Constants(context.Get(),evidence::ShaderStage::Pixel,analyzed);
        Check(unknown.buffers.size()==32&&std::all_of(unknown.buffers.begin(),unknown.buffers.end(),[](auto v){return v==0;})&&unknown.metadata.find("observed=false")!=std::string::npos,"opaque writes invalidate companion evidence without fabricating zero-valued constants");
        write(.25f/16,-.125f/16);
        const auto profile=automatic::Resolve(context.Get(),Stage::Pixel,analyzed).profile;
        Check(bool(profile),"valid observed writes recover automatic readiness");
        Candidate a;a.hash=1;a.stage=Stage::Pixel;a.width=a.height=16;a.lastSeen=3;a.frameCalls=1;a.inferred=profile;
        Candidate b=a;b.hash=2;b.width=b.height=32;
        Candidate cs=a;cs.stage=Stage::Compute;
        std::vector<Candidate> candidates{a,b,cs};
        Check(ChooseCandidate(candidates,3)==b.Key(),"automatic selection prefers the full-sized current-frame candidate");
        Check(ChooseCandidate(candidates,3,cs.Key())==cs.Key(),"pixel/compute hashes have independent persistent identities");
        std::reverse(candidates.begin(),candidates.end());Check(ChooseCandidate(candidates,3,a.Key())==a.Key(),"manual pins survive list sorting");
        Check(!ChooseCandidate(candidates,4,a.Key()),"an absent selected shader never falls back or replays a stale frame");
        Check(!ChooseCandidate(candidates,3,{Stage::Pixel,999}),"an unknown user pin cannot switch to another candidate");
        candidates={a};candidates[0].frameCalls=2;Check(!ChooseCandidate(candidates,3),"automatic selection rejects repeated same-shader views");
        Check(ChooseCandidate(candidates,3,a.Key())==a.Key(),"an explicit pin remains distinguishable from automatic selection");
        Check(ParseKey(KeyText(cs.Key()))==cs.Key()&&!ParseKey("PS:bad")&&ParseKey("auto")->hash==0,"stable selection serialization and invalid-key handling");
        context->ClearState();context->OMSetRenderTargets(1,&rt,dsv.Get());
        evidence::Frame linked;linked.Bindings(context.Get(),Stage::Pixel);
        Check(linked.metadata.find("ds0 resource=")!=std::string::npos&&linked.metadata.find("ds0_view format=40")!=std::string::npos,
            "companion bindings include the actual typed native depth view");
        auto psText=std::string(Source);ComPtr<ID3DBlob> psCode,vsCode,compileError;
        Hr(D3DCompile(psText.data(),psText.size(),nullptr,nullptr,nullptr,"main","ps_5_0",0,0,&psCode,&compileError));
        const char* vsText="cbuffer Raster:register(b3){float4 shift;}float4 main(uint id:SV_VertexID):SV_Position{return float4(float2(id,id)+shift.xy,0,1);}";
        Hr(D3DCompile(vsText,std::strlen(vsText),nullptr,nullptr,nullptr,"main","vs_5_0",0,0,&vsCode,&compileError));
        ComPtr<ID3D11PixelShader> ps;ComPtr<ID3D11VertexShader> vs;
        Hr(device->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),nullptr,&ps));
        Hr(device->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vs));
        const auto remember=[](ID3D11DeviceChild* shader,ID3DBlob* code){DrawObservationDx11::RememberShader(shader,DrawObservationDx11::ShaderByteHash(code->GetBufferPointer(),code->GetBufferSize()),code->GetBufferSize());automatic::Remember(shader,code->GetBufferPointer(),code->GetBufferSize());};
        remember(ps.Get(),psCode.Get());remember(vs.Get(),vsCode.Get());
        context->PSSetShader(ps.Get(),nullptr,0);context->VSSetShader(vs.Get(),nullptr,0);
        context1->PSSetConstantBuffers1(3,1,&bound,&first,&count);context1->VSSetConstantBuffers1(3,1,&bound,&first,&count);
        // Exercise the public Present clock with no SR/provider frame at all.
        // The real game that discovered this regression stayed at SR frame 0.
        const DrawObservationDx11::Consumer consumer{};
        DrawObservationDx11::Subscribe(DrawObservationDx11::Slot::TaaEvidence,&consumer,true);
        DrawObservationDx11::Present(context.Get());
        auto owner=*DrawObservationDx11::owner;
        const auto nextOwner=[&]{const auto old=owner;DrawObservationDx11::Present(context.Get());owner=*DrawObservationDx11::owner;
            Check(owner.frame==old.frame+1&&owner.epoch!=old.epoch,"public draw frame advances independently of absent SR evaluation");};
        evidence::RasterWatch watch{context.Get(),1,{{evidence::ResourceId(depth.Get()),5,{Stage::Pixel,42},"depth",{}}}};
        evidence::RasterWindow window;DrawObservationDx11::Draw indexed{DrawObservationDx11::Kind::IndexedInstanced,36,3,-2,5,7};
        unsigned exports=0,queued=0;std::string rasterMetadata;
        uint64_t now=1000;
        const auto begin=[&]{DrawObservationDx11::CapturedState state(context.Get());return window.Before(watch,context.Get(),indexed,owner,state,[&](auto,uint64_t,auto&& bytes,auto&&,const auto&){Check(!bytes.empty(),"raster shader export preserves actual bytecode");++exports;return true;},now);};
        const auto sink=[&](uint64_t,uint64_t,evidence::Frame&& record){++queued;rasterMetadata=record.metadata;Check(record.knownBytes==32,"raster CB snapshot observes actual PS and VS writes");return true;};
        auto cookie=begin();Check(cookie!=0&&exports==2,"indexed instanced native-depth draw captures its actual shaders");
        window.After(cookie,DrawObservationDx11::Completion::Consumed,watch,owner,sink,now);Check(!queued,"consumed draw never publishes an original writer record");
        nextOwner();cookie=begin();window.After(cookie,DrawObservationDx11::Completion::Abandoned,watch,owner,sink,now);Check(!queued,"abandoned draw never publishes an original writer record");
        nextOwner();cookie=begin();window.After(cookie,DrawObservationDx11::Completion::OriginalReturned,watch,owner,[](auto,auto,auto&&){return false;},now);
        for(unsigned burst=0;burst<4;++burst) {
            if(burst){now=1000+evidence::CaptureWindow::Offsets[burst];}
            for(unsigned i=0;i<9;++i){nextOwner();cookie=begin();window.After(cookie,DrawObservationDx11::Completion::OriginalReturned,watch,owner,sink,now);
                Check(!begin(),"same owner frame cannot replay a scene sample");}
            Check(queued<=(burst+1)*8,"each timed burst reserves a separate eight-frame budget");
        }
        Check(queued==32,"per-pair bounded retries retain all eight samples despite consumed draws or queue rejection");
        now=37000;nextOwner();Check(!begin(),"completed capture session does not query later frames");
        ++watch.revision;window.Reset();now=38000;cookie=begin();Check(cookie!=0,"new capture session resets its clock and byte budget");
        window.After(cookie,DrawObservationDx11::Completion::Abandoned,watch,owner,sink,now);
        Check(rasterMetadata.find("draw_kind=4")!=std::string::npos&&rasterMetadata.find("instances=5")!=std::string::npos&&rasterMetadata.find("completion=original_returned")!=std::string::npos,
            "raster evidence distinguishes indexed instancing and actual command return");
        evidence::RasterWindow unrelated;watch.targets[0].resource=evidence::ResourceId(motion.Get());DrawObservationDx11::CapturedState unrelatedState(context.Get());
        Check(!unrelated.Before(watch,context.Get(),indexed,owner,unrelatedState,[](auto,auto,auto&&,auto&&,const auto&){return true;}),"same-extent unrelated resource never authorizes raster capture");
        // A watch is first published by the resolve. Unrelated later draws must
        // not spend the next-frame producer's per-pair opportunities.
        evidence::RasterWindow lateProducer;unsigned lateQueued=0;
        const auto later=[&](uint64_t time){DrawObservationDx11::CapturedState state(context.Get());
            return lateProducer.Before(watch,context.Get(),indexed,owner,state,[](auto,auto,auto&&,auto&&,const auto&){return true;},time);};
        watch.startedMs=1000;
        for(unsigned i=0;i<12;++i){nextOwner();Check(!later(1000+i),"unrelated draws never arm a matched producer budget");}
        watch.targets[0].resource=evidence::ResourceId(depth.Get());
        nextOwner();cookie=later(1020);Check(cookie!=0,"first actual producer remains capturable after more than eight unrelated owner frames");
        lateProducer.After(cookie,DrawObservationDx11::Completion::OriginalReturned,watch,owner,
            [&](auto,auto,evidence::Frame&&){++lateQueued;return true;},1020);
        // The first actual CB snapshot cannot recover earlier CPU writes. Keep
        // the zero mask, then capture the next real write without a 5s wait.
        auto cbShadow=constants::Find(cb.Get());Check(bool(cbShadow),"fixture has an observed CB");
        {std::lock_guard lock(cbShadow->mutex);cbShadow->Invalidate();}
        nextOwner();cookie=later(1030);Check(cookie!=0,"first unknown raster constant snapshot is retained");
        lateProducer.After(cookie,DrawObservationDx11::Completion::OriginalReturned,watch,owner,
            [&](auto,auto,evidence::Frame&& f){++lateQueued;Check(f.knownBytes==0,"unknown CPU writes remain explicitly masked");return true;},1030);
        nextOwner();Check(!later(1040),"repeated unknown snapshots retry without exporting duplicate zero masks");
        write(.5f/16,-.25f/16);
        nextOwner();cookie=later(1050);Check(cookie!=0,"next actual CPU write is captured in the initial burst");
        lateProducer.After(cookie,DrawObservationDx11::Completion::OriginalReturned,watch,owner,
            [&](auto,auto,evidence::Frame&& f){++lateQueued;Check(f.knownBytes>=16,"retry preserves newly observed CPU bytes");return true;},1050);
        Check(lateQueued==3,"late producer and unknown warmup export only useful records");
        const auto lateReport=lateProducer.Report(owner.frame);
        Check(lateReport&&lateReport->find("target_miss=12")!=std::string::npos&&lateReport->find("unknown=2")!=std::string::npos,
            "raster report distinguishes unmatched draws and unknown writes");
        // A resource bound at MRT1 is not a writer when the actual PS only
        // declares MRT0. Check the real DXBC and effective blend state before
        // it can spend a high-priority motion pair or constant-buffer sample.
        ComPtr<ID3D11RenderTargetView> motionRtv;Hr(device->CreateRenderTargetView(scalar.Get(),nullptr,&motionRtv));
        ID3D11RenderTargetView* mrt[]{rtv.Get(),motionRtv.Get()};context->OMSetRenderTargets(2,mrt,nullptr);
        watch.targets[0]={evidence::ResourceId(scalar.Get()),owner.frame,{Stage::Pixel,42},"motion_hint",{},7,evidence::Subresources(scalarView.Get())};
        evidence::RasterWindow outputFilter;
        const auto filtered=[&]{DrawObservationDx11::CapturedState state(context.Get());return outputFilter.Before(watch,context.Get(),indexed,owner,state,[](auto,auto,auto&&,auto&&,const auto&){return true;},1100);};
        Check(!filtered(),"RT1 binding with an actual PS that only declares o0 spends no motion sample");
        const char* mrtSource="float4 main():SV_Target1{return float4(.5,.5,0,1);}";
        ComPtr<ID3DBlob> mrtCode;Hr(D3DCompile(mrtSource,std::strlen(mrtSource),nullptr,nullptr,nullptr,"main","ps_5_0",0,0,&mrtCode,&compileError));
        ComPtr<ID3D11PixelShader> mrtPs;Hr(device->CreatePixelShader(mrtCode->GetBufferPointer(),mrtCode->GetBufferSize(),nullptr,&mrtPs));remember(mrtPs.Get(),mrtCode.Get());context->PSSetShader(mrtPs.Get(),nullptr,0);
        D3D11_BLEND_DESC blendDesc{};blendDesc.RenderTarget[1].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
        ComPtr<ID3D11BlendState> sharedBlend;Hr(device->CreateBlendState(&blendDesc,&sharedBlend));context->OMSetBlendState(sharedBlend.Get(),nullptr,~0u);
        Check(!filtered(),"IndependentBlendEnable=false uses RT0's zero write mask for MRT1");
        blendDesc.IndependentBlendEnable=TRUE;ComPtr<ID3D11BlendState> independentBlend;Hr(device->CreateBlendState(&blendDesc,&independentBlend));context->OMSetBlendState(independentBlend.Get(),nullptr,~0u);
        cookie=filtered();Check(cookie!=0,"actual o1 declaration and independent RT1 write mask retain the real writer");
        const auto receipt=outputFilter.After(cookie,DrawObservationDx11::Completion::OriginalReturned,watch,owner,[](auto,auto,auto&&){return true;},1100);
        Check(receipt&&receipt->recordId&&receipt->cookie==cookie,"successful original return and export publish a typed raster receipt");
        evidence::RasterMailbox mailbox;mailbox.Publish(*receipt);
        evidence::Frame joinedFrame;joinedFrame.inputs[7]={evidence::ResourceId(scalar.Get()),16,16,D3D11_BIND_SHADER_RESOURCE,DXGI_FORMAT_R32_FLOAT,evidence::Subresources(scalarView.Get())};
        const auto capture=evidence::CaptureIdentity::From(watch,1100);const auto ownerId=evidence::OwnerIdentity::From(owner);
        const auto joined=mailbox.Join(capture,ownerId,{Stage::Pixel,42},joinedFrame);
        Check(joined.find(std::format("joined_raster_record={}",receipt->recordId))!=std::string::npos,"resolve references the exact successfully queued raster record");
        for(unsigned field=0;field<4;++field) {
            auto wrong=capture;if(field==0)++wrong.revision;else if(field==1)++wrong.startedMs;else if(field==2)++wrong.epoch;else ++wrong.burst;
            Check(mailbox.Join(wrong,ownerId,{Stage::Pixel,42},joinedFrame).empty(),"each capture identity component independently rejects stale raster joins");
        }
        for(unsigned field=0;field<4;++field) {
            auto wrong=ownerId;if(field==0)++wrong.context;else if(field==1)++wrong.thread;else if(field==2)++wrong.frame;else ++wrong.epoch;
            Check(mailbox.Join(capture,wrong,{Stage::Pixel,42},joinedFrame).empty(),"each owner identity component independently rejects cross-owner raster joins");
        }
        Check(mailbox.Join(capture,ownerId,{Stage::Pixel,43},joinedFrame).empty(),"same resource cannot join a different temporal candidate");
        for(unsigned field=0;field<4;++field) {
            auto wrong=joinedFrame;if(field==0)++wrong.inputs[7].resource;else if(field==1)++wrong.inputs[7].span.mip;else if(field==2)++wrong.inputs[7].span.firstSlice;else wrong.inputs[7].span.format=DXGI_FORMAT_R16_FLOAT;
            Check(mailbox.Join(capture,ownerId,{Stage::Pixel,42},wrong).empty(),"current resource and typed subresource view must match discovery");
        }
        for(unsigned field=0;field<6;++field) {
            nextOwner();cookie=filtered();Check(cookie!=0,"new owner admits a fresh writer attempt");
            auto changed=watch;auto changedOwner=owner;auto completion=DrawObservationDx11::Completion::OriginalReturned;
            if(field==0)++changed.revision;else if(field==1)++changed.startedMs;else if(field==2)++changed.captureEpoch;else if(field==3)++changedOwner.epoch;else if(field==4)changed.context=nullptr;else completion=DrawObservationDx11::Completion::Consumed;
            Check(!outputFilter.After(cookie,completion,changed,changedOwner,[](auto,auto,auto&&){return true;},1100),"changed capture/owner or consumed draw cannot publish a join receipt");
        }
        nextOwner();cookie=filtered();Check(cookie!=0,"queue rejection test admits the writer");
        Check(!outputFilter.After(cookie,DrawObservationDx11::Completion::OriginalReturned,watch,owner,[](auto,auto,auto&&){return false;},1100),"rejected export queue cannot create a successful raster receipt");
        nextOwner();cookie=filtered();Check(cookie!=0,"producer can be pending when the diagnostic request is cleared");
        outputFilter.Reset();mailbox.Reset();++watch.revision;nextOwner();cookie=filtered();
        Check(cookie!=0,"request cancellation clears pending and a new revision can capture again");
        Check(bool(outputFilter.After(cookie,DrawObservationDx11::Completion::OriginalReturned,watch,owner,[](auto,auto,auto&&){return true;},1100)),"re-enabled request completes its own actual writer");
        evidence::SampleBudget joinedBudget;
        Check(joinedBudget.Begin(1,analysis::CapturePolicy::SeedThenJoined)&&joinedBudget.Wants(false),"unknown seed gets one successful discovery record");joinedBudget.Queued(false);
        for(uint64_t frame=2;frame<200;++frame)Check(!joinedBudget.Begin(frame,analysis::CapturePolicy::SeedThenJoined),"no same-owner receipt spends no attempt after the seed");
        Check(joinedBudget.attempts==1&&joinedBudget.samples==1,"unjoined resolves preserve the bounded retry ledger");
        Check(joinedBudget.Begin(200,analysis::CapturePolicy::SeedThenJoined,true)&&joinedBudget.Wants(false,true),"new joined evidence remains valuable with an explicitly unknown CB mask");joinedBudget.Queued(false);
        for(uint64_t frame=201;frame<207;++frame){Check(joinedBudget.Begin(frame,analysis::CapturePolicy::SeedThenJoined,true),"joined samples share the original eight export slots");joinedBudget.Queued(true);}
        Check(joinedBudget.samples==8&&!joinedBudget.Begin(207,analysis::CapturePolicy::SeedThenJoined,true),"seed plus joined exports cannot enlarge the original sample cap");
        context->OMSetBlendState(nullptr,nullptr,~0u);context->OMSetRenderTargets(1,&rt,dsv.Get());context->PSSetShader(ps.Get(),nullptr,0);
        watch.startedMs=0;
        evidence::RasterWindow crowded;watch.targets[0]={evidence::ResourceId(depth.Get()),5,{Stage::Pixel,42},"depth",{}};
        unsigned crowdedQueued=0;std::string priorityMetadata;
        std::vector<ComPtr<ID3D11PixelShader>> crowdedShaders;
        const auto capturePair=[&]{DrawObservationDx11::CapturedState state(context.Get());const auto id=crowded.Before(watch,context.Get(),indexed,owner,state,[](auto,auto,auto&&,auto&&,const auto&){return true;},1000);
            crowded.After(id,DrawObservationDx11::Completion::OriginalReturned,watch,owner,[&](auto,auto,evidence::Frame&& record){++crowdedQueued;priorityMetadata=record.metadata;return true;},1000);return id;};
        ComPtr<ID3D11PixelShader> evictedPs;uint64_t smallestHash=~uint64_t(0);
        for(unsigned i=0;i<9;++i) {
            const auto code=std::format("float4 main():SV_Target0{{return float4({}.0/9,0,0,1);}}",i);
            ComPtr<ID3DBlob> differentCode;Hr(D3DCompile(code.data(),code.size(),nullptr,nullptr,nullptr,"main","ps_5_0",0,0,&differentCode,&compileError));
            ComPtr<ID3D11PixelShader> differentPs;Hr(device->CreatePixelShader(differentCode->GetBufferPointer(),differentCode->GetBufferSize(),nullptr,&differentPs));remember(differentPs.Get(),differentCode.Get());context->PSSetShader(differentPs.Get(),nullptr,0);
            const auto differentHash=DrawObservationDx11::ShaderByteHash(differentCode->GetBufferPointer(),differentCode->GetBufferSize());
            if(i<8&&differentHash<smallestHash){smallestHash=differentHash;evictedPs=differentPs;}
            Check(bool(capturePair())==(i<8),"loading scene shader pairs cannot exceed eight active slots");
            crowdedShaders.push_back(differentPs);
        }
        watch.targets[0].role="motion";
        Check(capturePair()!=0&&crowdedQueued==9&&priorityMetadata.find("role=motion")!=std::string::npos,"actual motion resource match can displace a lower priority loading pair");
        // The victim is ordered by shader key; rebind that exact pair with a
        // higher priority without discarding its already-attempted frame.
        context->PSSetShader(evictedPs.Get(),nullptr,0);
        Check(!capturePair()&&crowdedQueued==9,"a displaced pair cannot be recaptured in its already attempted owner frame");
        for(unsigned i=0;i<8;++i) {
            context->PSSetShader(crowdedShaders[i].Get(),nullptr,0);
            Check(!capturePair(),"evicted or upgraded shader pairs retain same-frame deduplication");
        }
        nextOwner();context->PSSetShader(evictedPs.Get(),nullptr,0);
        Check(capturePair()!=0&&crowdedQueued==10,"a displaced or active pair may capture a genuinely new owner frame");
        nextOwner();context->PSSetShader(crowdedShaders[0].Get(),nullptr,0);
        Check(capturePair()!=0,"an evicted pair may compete for a motion slot on the next source frame");
        // Exhaust A, evict it with a motion pair, then rebind A as motion.
        // An exhausted pair must not evict another still-useful low-priority pair.
        evidence::RasterWindow exhausted;const auto exhaustedCapture=[&]{DrawObservationDx11::CapturedState state(context.Get());
            const auto id=exhausted.Before(watch,context.Get(),indexed,owner,state,[](auto,auto,auto&&,auto&&,const auto&){return true;},1000);
            exhausted.After(id,DrawObservationDx11::Completion::OriginalReturned,watch,owner,[](auto,auto,auto&&){return true;},1000);return id;};
        watch.targets[0].role="depth";context->PSSetShader(evictedPs.Get(),nullptr,0);
        for(unsigned i=0;i<8;++i){nextOwner();Check(exhaustedCapture()!=0,"exhaust the first low-priority pair");}
        for(unsigned i=0;i<8;++i)if(crowdedShaders[i].Get()!=evictedPs.Get()){
            context->PSSetShader(crowdedShaders[i].Get(),nullptr,0);Check(exhaustedCapture()!=0,"populate remaining low-priority slots");}
        watch.targets[0].role="motion";context->PSSetShader(crowdedShaders[8].Get(),nullptr,0);
        Check(exhaustedCapture()!=0,"motion pair displaces the exhausted oldest-key pair");
        nextOwner();context->PSSetShader(evictedPs.Get(),nullptr,0);
        Check(!exhaustedCapture(),"exhausted rebind is rejected before changing priority or capacity");
        watch.targets[0].role="depth";
        for(unsigned i=0;i<8;++i)if(crowdedShaders[i].Get()!=evictedPs.Get()){
            context->PSSetShader(crowdedShaders[i].Get(),nullptr,0);Check(exhaustedCapture()!=0,"all useful low-priority pairs survive exhausted high-priority rebind");}
        automatic::Program retry(psCode->GetBufferPointer(),psCode->GetBufferSize());unsigned exportAttempts=0;
        const auto retryExport=[&](auto&& bytes,auto&& assembly,const auto&) {
            Check(bytes.size()==psCode->GetBufferSize()&&!std::memcmp(bytes.data(),psCode->GetBufferPointer(),bytes.size())&&!assembly.empty(),"queue retry retains exact shader bytes and assembly");
            return ++exportAttempts==2;
        };
        retry.Analyze(Stage::Pixel,42);
        retry.Analyze(Stage::Pixel,42,retryExport,{1,0});retry.Analyze(Stage::Pixel,42,retryExport,{1,0});
        Check(exportAttempts==1,"queue rejection cannot retry on every draw in one checkpoint");
        retry.Analyze(Stage::Pixel,42,retryExport,{1,1});retry.Analyze(Stage::Pixel,42,retryExport,{1,2});
        Check(exportAttempts==2,"later burst retries rejected shader export and accepted bytes are released once");
        Candidate logging;logging.reason="waiting for jitter";
        Check(logging.AnalysisDue(1),"first analysis is published");logging.RecordAnalysis(1);
        logging.reason="depth-tested TAA draw";
        Check(!logging.AnalysisDue(2),"early analysis changes remain throttled");
        Check(!logging.AnalysisDue(300),"analysis interval is measured from the last publication");
        Check(logging.AnalysisDue(301),"a stable pending rejection is eventually published after an early transition");logging.RecordAnalysis(301);
        Check(!logging.AnalysisDue(601),"unchanged rejection does not repeat on every interval");
        logging.inferred=Profile{};
        Check(logging.AnalysisDue(601),"readiness changes publish even when the reason text is unchanged");logging.RecordAnalysis(601);
        logging.reason="ready";logging.RecordAnalysis(602);
        Check(!logging.AnalysisDue(603)&&!logging.AnalysisDue(902),"publishing clears the pending state");
        evidence::CaptureWindow resize;resize.Enter(1,1000);
        Check(!resize.Rearm(35999),"initial evidence window cannot be interrupted");
        Check(resize.Rearm(37000)&&resize.epoch==1,"late extent may rearm diagnostics once");
        unsigned resizeFrames=0;
        for(unsigned i=0;i<9;++i)if(resize.Enter(50+i,37000))++resizeFrames;
        Check(resizeFrames==8&&!resize.Enter(59,39000),"resize burst has eight frames and a finite deadline");
        Check(resize.Rearm(40000)&&resize.epoch==2,"a second extent change may rearm");
        Check(!resize.Rearm(43000),"session cannot exceed two extent rearms");
        evidence::CaptureWindow schedule;unsigned admitted=0;
        for(unsigned burst=0;burst<4;++burst)for(unsigned i=0;i<100;++i)
            if(schedule.Enter(1000*burst+i,1000+evidence::CaptureWindow::Offsets[burst]))++admitted;
        Check(admitted==32&&!schedule.Enter(99999,36000),"eight-frame bursts remain bounded through loading and finish after 35 seconds");
        evidence::SampleBudget warmup;
        Check(warmup.Begin(1)&&warmup.Wants(false),"new candidate keeps its first masked unknown sample");warmup.Queued(false);
        Check(!warmup.Begin(1),"same-frame retry is suppressed");
        Check(warmup.Begin(2)&&!warmup.Wants(false),"unknown warmup does not flood the evidence queue");
        Check(warmup.Begin(3)&&warmup.Wants(true),"known next-frame bytes remain eligible without waiting for checkpoint");warmup.Queued(true);
        for(unsigned i=4;i<=16;++i)Check(warmup.Begin(i),"warmup has a finite consecutive-attempt budget");
        Check(!warmup.Begin(17)&&warmup.samples==2,"unknown retries cannot become an unbounded scan");
        std::vector<unsigned char> snapshotBytes,snapshotMask;uint64_t firstWrite=0,nextWrite=0;
        Check(constants::Snapshot(cb.Get(),0,16,snapshotBytes,snapshotMask,&firstWrite)&&firstWrite!=0,"known snapshot identifies the actual completed CPU write");
        write(.5f/16,-.25f/16);
        Check(constants::Snapshot(cb.Get(),0,16,snapshotBytes,snapshotMask,&nextWrite)&&nextWrite>firstWrite,"rewriting the same CB object creates a new write identity");
        constants::OpaqueWrite();
        Check(!constants::Snapshot(cb.Get(),0,16,snapshotBytes,snapshotMask,&nextWrite)&&nextWrite==0,"opaque writes cannot leave a valid CPU write identity");
        DrawObservationDx11::Subscribe(DrawObservationDx11::Slot::TaaEvidence,&consumer,false);DrawObservationDx11::Revoke();
        std::printf("PASS TAA automatic WARP and selection: %u checks\n",checks);return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
