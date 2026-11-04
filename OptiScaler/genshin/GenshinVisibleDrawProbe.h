// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GenshinVisibleOutput.h"
#include "GenshinDepthEvidence.h"
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <atomic>
#include <string>
#include <Util.h>

// R219's two observed edges only. Diagnostic observations never grant SR/NR
// admission, change native draws, bind state, or copy pixels back to the game.
namespace GenshinLegacy::VisibleOutput
{
inline constexpr uint64_t ScenePs=0x773B7BD7A2C6971Full,ToPicturePs=0xB58064BC5041A048ull;
inline constexpr GUID VisibleShaderKey={0x6e2a1a99,0xf537,0x4ba4,{0x89,0x18,0x12,0x4b,0x2b,0x47,0xb3,0x24}};
inline constexpr GUID VisibleBytecodeKey={0x6e2a1a99,0xf537,0x4ba4,{0x89,0x18,0x12,0x4b,0x2b,0x47,0xb3,0x25}};
struct VisibleShader {uint64_t hash=0,bytes=0,epoch=0;};
inline bool TargetPs(uint64_t hash) noexcept {return hash==ScenePs || hash==ToPicturePs;}
inline uintptr_t Canonical(IUnknown* object) noexcept
{
    ComPtr<IUnknown> id;if(object)object->QueryInterface(IID_PPV_ARGS(&id));
    return reinterpret_cast<uintptr_t>(id.Get());
}
inline VisibleShader ShaderFact(ID3D11DeviceChild* shader) noexcept
{
    VisibleShader fact;UINT size=sizeof(fact);
    if(!shader || FAILED(shader->GetPrivateData(VisibleShaderKey,&size,&fact)) || size!=sizeof(fact))return {};
    return fact;
}
inline void RecordVisibleShader(ID3D11DeviceChild* shader,const void* bytes,size_t size,
                                uint64_t epoch,bool vertex,uint64_t hash=0) noexcept
{
    if(!shader || !bytes || !size || !epoch || size>64*1024)return;
    if(!hash)hash=Hash(bytes,size);
    GenshinDepthEvidence::RetainVertex(shader,bytes,size,hash);
    // VS facts live on the shader object, so address reuse cannot inherit them.
    if(!vertex && !TargetPs(hash))return;
    const VisibleShader fact{hash,size,epoch};shader->SetPrivateData(VisibleShaderKey,sizeof(fact),&fact);
    if(vertex)return;
    static std::atomic_uint exported{0};const unsigned bit=hash==ScenePs?1:2;
    if(exported.fetch_or(bit,std::memory_order_relaxed)&bit)return;
    // Exact bounded bytes retained on the actual object; instruction text is
    // logged once per observed hash, without enabling broad/manual disk capture.
    const auto retained=shader->SetPrivateData(VisibleBytecodeKey,static_cast<UINT>(size),bytes);
    ComPtr<ID3DBlob> disassembly;
    const auto hr=D3DDisassemble(bytes,size,0,nullptr,&disassembly);
    try {
        std::string instructions;
        if(SUCCEEDED(hr) && disassembly && disassembly->GetBufferSize()<=128*1024)
            instructions.assign(static_cast<const char*>(disassembly->GetBufferPointer()),disassembly->GetBufferSize());
        if(!instructions.empty() && instructions.back()=='\0')instructions.pop_back();
        LOG_INFO("Genshin visible shader: pid={} epoch={} ps={:016X} bytes={} retained_hr={} disassemble_hr={} instructions_begin\n{}\ninstructions_end",
            GetCurrentProcessId(),epoch,hash,size,static_cast<int64_t>(retained),static_cast<int64_t>(hr),instructions);
    }catch(...){}
}

struct VisibleView
{
    uintptr_t resource=0,view=0;
    UINT width=0,height=0,mip=0,slice=0,subresource=0,samples=0,array=0,mips=0;
    DXGI_FORMAT resourceFormat=DXGI_FORMAT_UNKNOWN,viewFormat=DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D11Texture2D> texture;
    bool valid=false;
};
inline VisibleView Describe(ID3D11View* view,bool srv) noexcept
{
    VisibleView out;if(!view)return out;out.view=reinterpret_cast<uintptr_t>(view);
    ComPtr<ID3D11Resource> resource;view->GetResource(&resource);out.resource=Canonical(resource.Get());
    if(!resource || FAILED(resource.As(&out.texture)))return out;
    D3D11_TEXTURE2D_DESC d{};out.texture->GetDesc(&d);
    out.resourceFormat=d.Format;out.samples=d.SampleDesc.Count;out.array=d.ArraySize;out.mips=d.MipLevels;
    if(srv){ComPtr<ID3D11ShaderResourceView> typed;if(FAILED(view->QueryInterface(IID_PPV_ARGS(&typed))))return out;
        D3D11_SHADER_RESOURCE_VIEW_DESC v{};typed->GetDesc(&v);out.viewFormat=v.Format;
        if(v.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2D)out.mip=v.Texture2D.MostDetailedMip;
        else if(v.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2DARRAY && v.Texture2DArray.ArraySize==1){
            out.mip=v.Texture2DArray.MostDetailedMip;out.slice=v.Texture2DArray.FirstArraySlice;}
        else return out;
    }else {ComPtr<ID3D11RenderTargetView> typed;if(FAILED(view->QueryInterface(IID_PPV_ARGS(&typed))))return out;
        D3D11_RENDER_TARGET_VIEW_DESC v{};typed->GetDesc(&v);out.viewFormat=v.Format;
        if(v.ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2D)out.mip=v.Texture2D.MipSlice;
        else if(v.ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2DARRAY && v.Texture2DArray.ArraySize==1){
            out.mip=v.Texture2DArray.MipSlice;out.slice=v.Texture2DArray.FirstArraySlice;}
        else return out;
    }
    if(out.mip>=d.MipLevels || out.mip>=32 || out.slice>=d.ArraySize || d.SampleDesc.Count!=1 || d.SampleDesc.Quality)return out;
    out.width=(std::max)(1u,d.Width>>out.mip);out.height=(std::max)(1u,d.Height>>out.mip);
    out.subresource=D3D11CalcSubresource(out.mip,out.slice,d.MipLevels);out.valid=true;return out;
}
inline UINT PixelBytes(DXGI_FORMAT format) noexcept
{
    switch(format){
    case DXGI_FORMAT_R16G16B16A16_FLOAT:case DXGI_FORMAT_R16G16B16A16_UNORM:return 8;
    case DXGI_FORMAT_R8G8B8A8_UNORM:case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_UNORM:case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:return 4;
    default:return 0;}
}
class DrawProbe
{
    struct Sample {
        ComPtr<ID3D11Texture2D> staging;ComPtr<ID3D11Query> query;
        VisibleView source;UINT x=0,y=0,width=0,height=0,pixelBytes=0;
        uint64_t hash=0;HRESULT status=E_PENDING;bool queued=false,reported=false;
    };
    struct Edge {VisibleView input,output;UINT x=0,y=0;uint64_t draw=0,ps=0;bool armed=false,returned=false;};
    struct Window {
        uint64_t instance=0,generation=0,owner=0,device=0,config=0,openSerial=0,closeSerial=0;
        uintptr_t selected=0,intermediate=0,picture=0;
        UINT steps=0,polls=0;bool open=false,closed=false,pictureMatched=false,revoked=false;
        std::array<Edge,2> edges;std::array<Sample,7> samples;
    };
    std::array<Window,3> windows_{};UINT offered_=0;Window* current_=nullptr;
    uint64_t blockedOwner_=0,blockedDevice_=0,blockedConfig_=0;bool blocked_=false;
    static void SampleAt(ID3D11DeviceContext* context,Sample& out,const VisibleView& view,UINT centerX,UINT centerY) noexcept
    {
        if(out.queued || !context || !view.valid || !view.texture)return;
        out.source=view;out.pixelBytes=PixelBytes(view.viewFormat);
        if(!out.pixelBytes){out.status=E_NOTIMPL;return;}
        out.width=(std::min)(8u,view.width);out.height=(std::min)(8u,view.height);
        out.x=(std::min)(centerX>out.width/2?centerX-out.width/2:0u,view.width-out.width);
        out.y=(std::min)(centerY>out.height/2?centerY-out.height/2:0u,view.height-out.height);
        D3D11_TEXTURE2D_DESC desc{};view.texture->GetDesc(&desc);
        desc.Width=out.width;desc.Height=out.height;desc.ArraySize=desc.MipLevels=desc.SampleDesc.Count=1;
        desc.SampleDesc.Quality=0;desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Device> device;context->GetDevice(&device);
        out.status=device->CreateTexture2D(&desc,nullptr,&out.staging);if(FAILED(out.status))return;
        const D3D11_QUERY_DESC query{D3D11_QUERY_EVENT,0};out.status=device->CreateQuery(&query,&out.query);if(FAILED(out.status))return;
        const D3D11_BOX box{out.x,out.y,0,out.x+out.width,out.y+out.height,1};
        context->CopySubresourceRegion(out.staging.Get(),0,0,0,0,view.texture.Get(),view.subresource,&box);
        context->End(out.query.Get());out.queued=true;out.status=E_PENDING;
    }
    static void ViewLog(const Window& w,unsigned edge,const char* role,const VisibleView& v) noexcept
    {
        LOG_INFO("Genshin visible view: pid={} instance={} generation={} edge={} role={} resource={} view={} subresource={} mip={} slice={} extent={}x{} resource_format={} view_format={} samples={} array={} mips={} valid={}",
            GetCurrentProcessId(),w.instance,w.generation,edge,role,v.resource,v.view,v.subresource,v.mip,v.slice,v.width,v.height,
            static_cast<unsigned>(v.resourceFormat),static_cast<unsigned>(v.viewFormat),v.samples,v.array,v.mips,v.valid);
    }
public:
    // Logical revocation only: no COM release or GPU work on a foreign thread.
    // Closed samples remain owned until their query completes on owner-thread Poll.
    void Revoke() noexcept
    {
        if(offered_ && !windows_[offered_-1].revoked){const auto& last=windows_[offered_-1];blockedOwner_=last.owner;
            blockedDevice_=last.device;blockedConfig_=last.config;blocked_=true;}
        for(auto& w:windows_)if(w.open || w.closed){
            w.open=false;w.closed=true;w.revoked=true;w.pictureMatched=false;
            w.instance=w.generation=w.owner=w.device=w.config=w.openSerial=w.closeSerial=0;
            w.selected=w.intermediate=w.picture=0;
            for(auto& edge:w.edges){edge.armed=edge.returned=false;edge.draw=edge.ps=0;
                edge.input.resource=edge.input.view=edge.output.resource=edge.output.view=0;}
            for(auto& sample:w.samples)sample.source.resource=sample.source.view=0;
        }
        current_=nullptr;
    }
    bool Pending()const noexcept
    {
        for(const auto& w:windows_)for(const auto& sample:w.samples)
            if(sample.queued && !sample.reported)return true;
        return false;
    }
    bool Open()const noexcept {return current_ && current_->open;}
    bool Active()const noexcept {return current_ && current_->open && current_->steps<64;}
    void Begin(ID3D11Resource* selected,uint64_t instance,uint64_t generation,uint64_t owner,
               uint64_t device,uint64_t config,uint64_t serial) noexcept
    {
        if(offered_>=windows_.size() || !selected || !instance || !generation)return;
        if(blocked_ && owner==blockedOwner_ && device==blockedDevice_ && config==blockedConfig_)return;
        blocked_=false;
        // Do not silently reassign an unfinished Present window to a newer SR generation.
        if(current_ && current_->open)return;
        auto& w=windows_[offered_++];w.instance=instance;w.generation=generation;w.owner=owner;
        w.device=device;w.config=config;w.openSerial=serial;w.selected=Canonical(selected);w.open=true;current_=&w;
        LOG_INFO("Genshin visible window: pid={} phase=open instance={} generation={} owner={} device={} config={} serial={} selected={} max_draws=64 max_windows=3",
            GetCurrentProcessId(),instance,generation,owner,device,config,serial,w.selected);
    }
    void Before(ID3D11DeviceContext* context,uint64_t ps,uint64_t draw,UINT count,bool countKnown,unsigned kind) noexcept
    {
        if(!Active() || !context)return;auto& w=*current_;++w.steps;
        if(!TargetPs(ps))return;const unsigned index=ps==ScenePs?0:1;auto& edge=w.edges[index];
        if(edge.armed || edge.returned || (index==1 && !w.intermediate))return;
        ComPtr<ID3D11ShaderResourceView> srv;ComPtr<ID3D11RenderTargetView> rtv;ComPtr<ID3D11DepthStencilView> dsv;
        context->PSGetShaderResources(0,1,&srv);context->OMGetRenderTargets(1,&rtv,&dsv);
        auto input=Describe(srv.Get(),true),output=Describe(rtv.Get(),false);
        const auto expected=index==0?w.selected:w.intermediate;
        if(!input.valid || !output.valid || input.resource!=expected)return;
        edge.input=std::move(input);edge.output=std::move(output);edge.draw=draw;edge.ps=ps;edge.armed=true;
        ComPtr<ID3D11VertexShader> vs;context->VSGetShader(&vs,nullptr,nullptr);const auto vertex=ShaderFact(vs.Get());
        UINT viewportCount=D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        std::array<D3D11_VIEWPORT,D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> viewports{};
        context->RSGetViewports(&viewportCount,viewports.data());
        edge.x=edge.output.width/2;edge.y=edge.output.height/2;
        if(viewportCount==1){const auto& v=viewports[0];
            const auto x=v.TopLeftX+v.Width/2,y=v.TopLeftY+v.Height/2;
            if(x>=0 && x<edge.output.width && y>=0 && y<edge.output.height){edge.x=static_cast<UINT>(x);edge.y=static_cast<UINT>(y);}}
        LOG_INFO("Genshin visible draw: pid={} instance={} generation={} edge={} draw={} ps={:016X} vs={:016X} vs_epoch={} owner={} device={} config={} serial={} kind={} count={} count_known={} indexed={} viewports={} viewport_coordinate_policy=output_center_or_single_viewport_center input_coordinate_policy=view_center full_pixel_use=UNKNOWN",
            GetCurrentProcessId(),w.instance,w.generation,index,draw,ps,vertex.hash,vertex.epoch,w.owner,w.device,w.config,w.openSerial,
            kind,count,countKnown,kind==2 || kind==4 || kind==6,viewportCount);
        for(UINT i=0;i<viewportCount;++i){const auto& v=viewports[i];
            LOG_INFO("Genshin visible viewport: generation={} edge={} index={} xy={},{} extent={}x{} depth={},{}",w.generation,index,i,v.TopLeftX,v.TopLeftY,v.Width,v.Height,v.MinDepth,v.MaxDepth);}
        UINT scissorCount=D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        std::array<D3D11_RECT,D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> scissors{};context->RSGetScissorRects(&scissorCount,scissors.data());
        ComPtr<ID3D11RasterizerState> raster;context->RSGetState(&raster);D3D11_RASTERIZER_DESC rd{};
        rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_BACK;rd.DepthClipEnable=TRUE;if(raster)raster->GetDesc(&rd);
        for(UINT i=0;i<scissorCount;++i){const auto& r=scissors[i];LOG_INFO("Genshin visible scissor: generation={} edge={} index={} rect={},{},{},{} enabled={}",w.generation,index,i,r.left,r.top,r.right,r.bottom,raster && rd.ScissorEnable);}
        ComPtr<ID3D11BlendState> blend;FLOAT blendFactor[4]{};UINT mask=0;context->OMGetBlendState(&blend,blendFactor,&mask);
        D3D11_BLEND_DESC bd{};auto& defaults=bd.RenderTarget[0];defaults.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
        defaults.SrcBlend=defaults.SrcBlendAlpha=D3D11_BLEND_ONE;defaults.DestBlend=defaults.DestBlendAlpha=D3D11_BLEND_ZERO;
        defaults.BlendOp=defaults.BlendOpAlpha=D3D11_BLEND_OP_ADD;if(blend)blend->GetDesc(&bd);const auto& rt=bd.RenderTarget[0];
        ComPtr<ID3D11DepthStencilState> depth;UINT reference=0;context->OMGetDepthStencilState(&depth,&reference);
        D3D11_DEPTH_STENCIL_DESC dd{};dd.DepthEnable=TRUE;dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;dd.DepthFunc=D3D11_COMPARISON_LESS;
        dd.StencilReadMask=dd.StencilWriteMask=D3D11_DEFAULT_STENCIL_READ_MASK;
        dd.FrontFace={D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_COMPARISON_ALWAYS};dd.BackFace=dd.FrontFace;if(depth)depth->GetDesc(&dd);
        LOG_INFO("Genshin visible coverage: generation={} edge={} blend={} write_mask={:X} sample_mask={:X} src={} dst={} op={} src_a={} dst_a={} op_a={} alpha_to_coverage={} independent_blend={} depth={} depth_write={} depth_func={} stencil={} stencil_read={} stencil_write={} stencil_ref={} front_func={} back_func={} fill={} cull={} scissor_enabled={} dsv={} blend_factor={},{},{},{} coverage=UNKNOWN",
            w.generation,index,static_cast<bool>(rt.BlendEnable),rt.RenderTargetWriteMask,mask,static_cast<unsigned>(rt.SrcBlend),static_cast<unsigned>(rt.DestBlend),static_cast<unsigned>(rt.BlendOp),
            static_cast<unsigned>(rt.SrcBlendAlpha),static_cast<unsigned>(rt.DestBlendAlpha),static_cast<unsigned>(rt.BlendOpAlpha),static_cast<bool>(bd.AlphaToCoverageEnable),static_cast<bool>(bd.IndependentBlendEnable),
            static_cast<bool>(dd.DepthEnable),static_cast<unsigned>(dd.DepthWriteMask),static_cast<unsigned>(dd.DepthFunc),static_cast<bool>(dd.StencilEnable),dd.StencilReadMask,dd.StencilWriteMask,reference,
            static_cast<unsigned>(dd.FrontFace.StencilFunc),static_cast<unsigned>(dd.BackFace.StencilFunc),static_cast<unsigned>(rd.FillMode),static_cast<unsigned>(rd.CullMode),raster && rd.ScissorEnable,
            reinterpret_cast<uintptr_t>(dsv.Get()),blendFactor[0],blendFactor[1],blendFactor[2],blendFactor[3]);
        ViewLog(w,index,"t0",edge.input);ViewLog(w,index,"rtv0",edge.output);
        SampleAt(context,w.samples[index*3],edge.input,edge.input.width/2,edge.input.height/2);
        SampleAt(context,w.samples[index*3+1],edge.output,edge.x,edge.y);
    }
    void Returned(ID3D11DeviceContext* context,uint64_t ps,uint64_t draw) noexcept
    {
        if(!current_ || !current_->open || !context || !TargetPs(ps))return;auto& w=*current_;const unsigned index=ps==ScenePs?0:1;auto& e=w.edges[index];
        if(!e.armed || e.returned || e.draw!=draw)return;
        ComPtr<ID3D11RenderTargetView> output;context->OMGetRenderTargets(1,&output,nullptr);const auto actual=Describe(output.Get(),false);
        if(actual.resource!=e.output.resource || actual.view!=e.output.view || actual.subresource!=e.output.subresource){e.armed=false;return;}
        SampleAt(context,w.samples[index*3+2],e.output,e.x,e.y);e.returned=true;e.armed=false;
        if(index==0)w.intermediate=e.output.resource;else w.picture=e.output.resource;
        LOG_INFO("Genshin visible returned: pid={} instance={} generation={} edge={} draw={} ps={:016X} input={} output={} serial={} original_draw_returned=true dataflow=UNKNOWN",
            GetCurrentProcessId(),w.instance,w.generation,index,draw,ps,e.input.resource,e.output.resource,w.openSerial);
    }
    void Present(ID3D11DeviceContext* context,IDXGISwapChain* swapchain,uint64_t serial) noexcept
    {
        if(!current_ || !current_->open)return;auto& w=*current_;w.open=false;w.closed=true;w.closeSerial=serial;
        ComPtr<ID3D11Texture2D> picture;ComPtr<IDXGISwapChain3> indexed;UINT index=0;bool exactIndex=false;
        if(swapchain && SUCCEEDED(swapchain->QueryInterface(IID_PPV_ARGS(&indexed)))){index=indexed->GetCurrentBackBufferIndex();exactIndex=true;}
        if(swapchain && SUCCEEDED(swapchain->GetBuffer(index,IID_PPV_ARGS(&picture)))){
            w.pictureMatched=w.picture && Canonical(picture.Get())==w.picture;
            if(w.pictureMatched)SampleAt(context,w.samples[6],w.edges[1].output,w.edges[1].x,w.edges[1].y);
        }
        LOG_INFO("Genshin visible window: pid={} phase=present-close instance={} generation={} owner={} device={} config={} open_serial={} close_serial={} scene_returned={} picture_returned={} observed_picture={} actual_backbuffer={} picture_match={} index={} index_exact={} same_window=true same_frame_execution=UNKNOWN final_visible=UNKNOWN",
            GetCurrentProcessId(),w.instance,w.generation,w.owner,w.device,w.config,w.openSerial,serial,w.edges[0].returned,w.edges[1].returned,
            w.picture,Canonical(picture.Get()),w.pictureMatched,index,exactIndex);
        current_=nullptr;
    }
    void Poll(ID3D11DeviceContext* context) noexcept
    {
        if(!context)return;
        for(auto& w:windows_)if(w.closed){if(w.polls<120)++w.polls;
            for(unsigned i=0;i<w.samples.size();++i){auto& s=w.samples[i];if(s.reported || (!w.revoked && !s.source.resource))continue;
                if(w.revoked){
                    if(s.queued){BOOL done=FALSE;const auto hr=context->GetData(s.query.Get(),&done,sizeof(done),D3D11_ASYNC_GETDATA_DONOTFLUSH);
                        if(hr==S_FALSE || (SUCCEEDED(hr) && !done))continue;}
                    s.reported=true;s.staging.Reset();s.query.Reset();s.source.texture.Reset();continue;
                }
                if(s.queued){BOOL done=FALSE;auto hr=context->GetData(s.query.Get(),&done,sizeof(done),D3D11_ASYNC_GETDATA_DONOTFLUSH);
                    // A poll budget never permits releasing in-flight GPU resources.
                    if(hr==S_FALSE || (SUCCEEDED(hr) && !done))continue;
                    if(SUCCEEDED(hr) && done){D3D11_MAPPED_SUBRESOURCE mapped{};hr=context->Map(s.staging.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);
                        if(hr==DXGI_ERROR_WAS_STILL_DRAWING)continue;
                        if(SUCCEEDED(hr)){s.hash=14695981039346656037ull;
                            for(UINT row=0;row<s.height;++row)s.hash=Hash(static_cast<const uint8_t*>(mapped.pData)+static_cast<size_t>(row)*mapped.RowPitch,s.width*s.pixelBytes,s.hash);
                            context->Unmap(s.staging.Get(),0);}}
                    else if(SUCCEEDED(hr))hr=E_PENDING;s.status=hr;
                }
                LOG_INFO("Genshin visible pixels: pid={} instance={} generation={} sample={} owner={} device={} config={} open_serial={} close_serial={} resource={} view={} subresource={} format={} xy={},{} extent={}x{} bytes={} hr={} hash={:016X} picture_match={} coordinate_transform=UNKNOWN final_visible=UNKNOWN",
                    GetCurrentProcessId(),w.instance,w.generation,i,w.owner,w.device,w.config,w.openSerial,w.closeSerial,s.source.resource,s.source.view,s.source.subresource,
                    static_cast<unsigned>(s.source.viewFormat),s.x,s.y,s.width,s.height,s.width*s.height*s.pixelBytes,static_cast<int64_t>(s.status),s.hash,w.pictureMatched);
                s.reported=true;s.staging.Reset();s.query.Reset();s.source.texture.Reset();
            }
            // No query retries, COM references or bookkeeping after this bounded window drains.
            bool pending=false;for(const auto& sample:w.samples)pending=pending || (sample.queued && !sample.reported);
            if(!pending){w.polls=120;for(auto& edge:w.edges){edge.input.texture.Reset();edge.output.texture.Reset();}}
        }
    }
};
}
