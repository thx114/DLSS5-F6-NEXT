// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "TaaAutoDx11.h"
#include "TaaSelection.h"
#include "TaaDepthStencilDx11.h"
#include <resource_tracking/DrawObservation_Dx11.h>
#include <format>

namespace taa::evidence {
// Four wall-clock checkpoints bound capture to 35 seconds and 32 exports per
// shader/pair. Each pair has its own short warmup, so unmatched draws cannot
// consume a later producer's samples. Late extent probes are separately bounded.
struct CaptureWindow {
    static constexpr uint64_t Offsets[]{0,5000,15000,30000};
    static constexpr uint64_t Duration=35000,Frames=8;
    static constexpr uint64_t RearmDuration=2000;
    static constexpr unsigned MaxRearms=2;
    uint64_t startedMs=0;
    unsigned epoch=0;
    bool Rearm(uint64_t now) {
        if(!Complete(now)||epoch>=MaxRearms)return false;
        startedMs=now;++epoch;firstFrames.fill(~uint64_t(0));return true;
    }
    std::array<uint64_t,4> firstFrames{~uint64_t(0),~uint64_t(0),~uint64_t(0),~uint64_t(0)};
    bool Complete(uint64_t now) const {return startedMs&&now>=startedMs&&now-startedMs>=(epoch?RearmDuration:Duration);}
    unsigned Burst(uint64_t now) const {
        unsigned burst=0;while(burst+1<std::size(Offsets)&&now>=startedMs&&now-startedMs>=Offsets[burst+1])++burst;
        return epoch*unsigned(std::size(Offsets))+(epoch?0u:burst);
    }
    std::optional<unsigned> Active(uint64_t now) {
        if(!startedMs)startedMs=now;
        if(now<startedMs||Complete(now))return {};
        return Burst(now);
    }
    std::optional<unsigned> Enter(uint64_t frame,uint64_t now) {
        const auto active=Active(now);if(!active)return {};
        const auto burst=*active;
        auto& first=firstFrames[burst%std::size(Offsets)];if(first==~uint64_t(0))first=frame;
        return frame>=first&&frame-first<Frames?std::optional<unsigned>(burst):std::nullopt;
    }
};
// Each actual shader/pair gets a bounded warmup after its first eligible draw.
// Unrelated draws cannot spend its frames. The first unknown record is retained;
// further unknown writes retry without filling the export queue with zero masks.
struct SampleBudget {
    static constexpr unsigned MaxAttempts=16;
    uint64_t lastFrame=~uint64_t(0);
    unsigned attempts=0,samples=0;
    bool unknownQueued=false;
    bool CanBegin(uint64_t frame) const {return lastFrame!=frame&&attempts<MaxAttempts&&samples<CaptureWindow::Frames;}
    bool Begin(uint64_t frame,analysis::CapturePolicy policy=analysis::CapturePolicy::Consecutive,bool joined=false) {
        if(!CanBegin(frame)||(policy==analysis::CapturePolicy::SeedThenJoined&&samples&&!joined))return false;
        lastFrame=frame;++attempts;return true;
    }
    bool Wants(bool known,bool joined=false) const {return known||joined||!unknownQueued;}
    void Queued(bool known) {++samples;unknownQueued|=!known;}
};
// Subresource ranges are diagnostic identities. A matching resource object
// alone cannot join a mip/array producer to a temporal input view.
struct ViewSpan {
    unsigned mip=0,levels=0,firstSlice=0,slices=0,dimension=0;
    DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
    bool Known() const {return levels&&slices;}
    bool operator==(const ViewSpan&) const=default;
    bool Intersects(const ViewSpan& other) const {
        return Known()&&other.Known()&&uint64_t(mip)<uint64_t(other.mip)+other.levels&&
            uint64_t(other.mip)<uint64_t(mip)+levels&&uint64_t(firstSlice)<uint64_t(other.firstSlice)+other.slices&&
            uint64_t(other.firstSlice)<uint64_t(firstSlice)+slices;
    }
};
template<class View> inline ViewSpan Subresources(View*) {return {};}
inline ViewSpan Subresources(ID3D11ShaderResourceView* view) {
    if(!view)return {};D3D11_SHADER_RESOURCE_VIEW_DESC d{};view->GetDesc(&d);
    ViewSpan s{};s.dimension=unsigned(d.ViewDimension);s.format=d.Format;
    switch(d.ViewDimension) {
    case D3D11_SRV_DIMENSION_TEXTURE2D:s.mip=d.Texture2D.MostDetailedMip;s.levels=d.Texture2D.MipLevels;s.slices=1;break;
    case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:s.mip=d.Texture2DArray.MostDetailedMip;s.levels=d.Texture2DArray.MipLevels;s.firstSlice=d.Texture2DArray.FirstArraySlice;s.slices=d.Texture2DArray.ArraySize;break;
    case D3D11_SRV_DIMENSION_TEXTURE2DMS:s.levels=s.slices=1;break;
    case D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY:s.levels=1;s.firstSlice=d.Texture2DMSArray.FirstArraySlice;s.slices=d.Texture2DMSArray.ArraySize;break;
    default:break;
    }
    return s;
}
inline ViewSpan Subresources(ID3D11RenderTargetView* view) {
    if(!view)return {};D3D11_RENDER_TARGET_VIEW_DESC d{};view->GetDesc(&d);
    ViewSpan s{};s.dimension=unsigned(d.ViewDimension);s.format=d.Format;s.levels=1;
    switch(d.ViewDimension) {
    case D3D11_RTV_DIMENSION_TEXTURE2D:s.mip=d.Texture2D.MipSlice;s.slices=1;break;
    case D3D11_RTV_DIMENSION_TEXTURE2DARRAY:s.mip=d.Texture2DArray.MipSlice;s.firstSlice=d.Texture2DArray.FirstArraySlice;s.slices=d.Texture2DArray.ArraySize;break;
    case D3D11_RTV_DIMENSION_TEXTURE2DMS:s.slices=1;break;
    case D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY:s.firstSlice=d.Texture2DMSArray.FirstArraySlice;s.slices=d.Texture2DMSArray.ArraySize;break;
    default:break;
    }
    return s;
}
inline ViewSpan Subresources(ID3D11DepthStencilView* view) {
    if(!view)return {};D3D11_DEPTH_STENCIL_VIEW_DESC d{};view->GetDesc(&d);
    ViewSpan s{};s.dimension=unsigned(d.ViewDimension);s.format=d.Format;s.levels=1;
    switch(d.ViewDimension) {
    case D3D11_DSV_DIMENSION_TEXTURE2D:s.mip=d.Texture2D.MipSlice;s.slices=1;break;
    case D3D11_DSV_DIMENSION_TEXTURE2DARRAY:s.mip=d.Texture2DArray.MipSlice;s.firstSlice=d.Texture2DArray.FirstArraySlice;s.slices=d.Texture2DArray.ArraySize;break;
    case D3D11_DSV_DIMENSION_TEXTURE2DMS:s.slices=1;break;
    case D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY:s.firstSlice=d.Texture2DMSArray.FirstArraySlice;s.slices=d.Texture2DMSArray.ArraySize;break;
    default:break;
    }
    return s;
}
enum class ShaderStage { Vertex,Pixel,Compute };
inline constexpr GUID ResourceKey={0xf6aa0004,0x928b,0x4cb7,{0xa5,0x91,0xda,0x11,0x71,0x21,0x83,0x05}};
inline uint64_t ResourceId(ID3D11Resource* resource) {
    if(!resource)return 0;
    uint64_t id=0;UINT size=sizeof(id);
    if(SUCCEEDED(resource->GetPrivateData(ResourceKey,&size,&id))&&size==sizeof(id)&&id)return id;
    static std::atomic<uint64_t> serial{0};id=++serial;
    return SUCCEEDED(resource->SetPrivateData(ResourceKey,sizeof(id),&id))?id:0;
}
struct Frame {
    struct TextureBinding { uint64_t resource=0;unsigned width=0,height=0,bindFlags=0;DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;ViewSpan span{}; };
    std::string metadata;
    std::vector<unsigned char> buffers;
    size_t knownBytes=0;
    uint64_t recordId=0;
    std::array<TextureBinding,128> inputs{};
    std::array<std::string,128> inputViews{};
    void Constants(ID3D11DeviceContext* context,ShaderStage stage,const analysis::Result& program) {
        Microsoft::WRL::ComPtr<ID3D11DeviceContext1> c;
        if(FAILED(context->QueryInterface(IID_PPV_ARGS(&c)))) {metadata+="constant_buffers=unavailable\n";return;}
        std::array<ID3D11Buffer*,14> raw{};std::array<UINT,14> first{},count{};
        const char* name=stage==ShaderStage::Vertex?"VS":stage==ShaderStage::Pixel?"PS":"CS";
        if(stage==ShaderStage::Vertex)c->VSGetConstantBuffers1(0,14,raw.data(),first.data(),count.data());
        else if(stage==ShaderStage::Pixel)c->PSGetConstantBuffers1(0,14,raw.data(),first.data(),count.data());
        else c->CSGetConstantBuffers1(0,14,raw.data(),first.data(),count.data());
        std::array<Microsoft::WRL::ComPtr<ID3D11Buffer>,14> held;
        for(unsigned i=0;i<14;++i)held[i].Attach(raw[i]);
        for(const auto& declaration:program.constantBuffers) {
            const auto slot=declaration.slot;if(slot>=14)continue;
            const unsigned length=(std::min)(declaration.bytes,count[slot]*16u);
            if(!held[slot]||!length) {metadata+=std::format("{}_cb{}=unbound\n",name,slot);continue;}
            // One frame is bounded independently of shader/CB declarations.
            if(buffers.size()+size_t(length)*2>128*1024) {metadata+="constant_buffers=byte_budget\n";break;}
            std::vector<unsigned char> bytes,known;
            uint64_t writeSerial=0,constantEpoch=0;
            const bool observed=constants::Snapshot(held[slot].Get(),first[slot]*16,length,bytes,known,&writeSerial,&constantEpoch);
            const auto offset=buffers.size();const auto knownCount=std::count(known.begin(),known.end(),1);
            knownBytes+=knownCount;
            metadata+=std::format("{}_cb{} resource={} first_byte={} declared_bytes={} captured_bytes={} observed={} known_bytes={} write_serial={} constant_epoch={} binary_offset={} mask_offset={}\n",
                name,slot,ResourceId(held[slot].Get()),first[slot]*16,declaration.bytes,length,observed,knownCount,writeSerial,constantEpoch,offset,offset+length);
            buffers.insert(buffers.end(),bytes.begin(),bytes.end());buffers.insert(buffers.end(),known.begin(),known.end());
        }
    }
    template<class View> TextureBinding ViewIdentity(const char* name,unsigned slot,View* view) {
        if(!view)return {};
        Microsoft::WRL::ComPtr<ID3D11Resource> resource;view->GetResource(&resource);
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;if(FAILED(resource.As(&texture)))return {};
        D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);
        const auto id=ResourceId(resource.Get());
        metadata+=std::format("{}{} resource={} width={} height={} format={} samples={}\n",name,slot,id,d.Width,d.Height,unsigned(d.Format),d.SampleDesc.Count);
        if(d.BindFlags&D3D11_BIND_DEPTH_STENCIL) {
            int convention=-1;UINT bytes=sizeof(convention);
            const bool known=SUCCEEDED(texture->GetPrivateData(automatic::DepthKey,&bytes,&convention))&&bytes==sizeof(convention)&&convention>=0;
            metadata+=std::format("{}{}_depth_convention known={} inverted={} source=ordered_native_clear_only\n",name,slot,known,known?convention:-1);
        }
        metadata+=ViewDescription(name,slot,view);
        return {id,d.Width,d.Height,d.BindFlags,d.Format,Subresources(view)};
    }
    static std::string ViewDescription(const char* name,unsigned slot,ID3D11ShaderResourceView* view) {
        D3D11_SHADER_RESOURCE_VIEW_DESC v{};view->GetDesc(&v);
        unsigned mip=0,levels=0,first=0,count=0;
        switch(v.ViewDimension) {
        case D3D11_SRV_DIMENSION_TEXTURE2D:mip=v.Texture2D.MostDetailedMip;levels=v.Texture2D.MipLevels;count=1;break;
        case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:mip=v.Texture2DArray.MostDetailedMip;levels=v.Texture2DArray.MipLevels;first=v.Texture2DArray.FirstArraySlice;count=v.Texture2DArray.ArraySize;break;
        case D3D11_SRV_DIMENSION_TEXTURE2DMS:count=1;break;
        case D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY:first=v.Texture2DMSArray.FirstArraySlice;count=v.Texture2DMSArray.ArraySize;break;
        default:break;
        }
        return std::format("{}{}_view format={} dimension={} mip={} mip_levels={} first_slice={} array_size={}\n",name,slot,unsigned(v.Format),unsigned(v.ViewDimension),mip,levels,first,count);
    }
    static std::string ViewDescription(const char* name,unsigned slot,ID3D11RenderTargetView* view) {
        D3D11_RENDER_TARGET_VIEW_DESC v{};view->GetDesc(&v);unsigned mip=0,first=0,count=0;
        switch(v.ViewDimension) {
        case D3D11_RTV_DIMENSION_TEXTURE2D:mip=v.Texture2D.MipSlice;count=1;break;
        case D3D11_RTV_DIMENSION_TEXTURE2DARRAY:mip=v.Texture2DArray.MipSlice;first=v.Texture2DArray.FirstArraySlice;count=v.Texture2DArray.ArraySize;break;
        case D3D11_RTV_DIMENSION_TEXTURE2DMS:count=1;break;
        case D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY:first=v.Texture2DMSArray.FirstArraySlice;count=v.Texture2DMSArray.ArraySize;break;
        default:break;
        }
        return std::format("{}{}_view format={} dimension={} mip={} first_slice={} array_size={}\n",name,slot,unsigned(v.Format),unsigned(v.ViewDimension),mip,first,count);
    }
    static std::string ViewDescription(const char* name,unsigned slot,ID3D11DepthStencilView* view) {
        D3D11_DEPTH_STENCIL_VIEW_DESC v{};view->GetDesc(&v);unsigned mip=0,first=0,count=0;
        switch(v.ViewDimension) {
        case D3D11_DSV_DIMENSION_TEXTURE2D:mip=v.Texture2D.MipSlice;count=1;break;
        case D3D11_DSV_DIMENSION_TEXTURE2DARRAY:mip=v.Texture2DArray.MipSlice;first=v.Texture2DArray.FirstArraySlice;count=v.Texture2DArray.ArraySize;break;
        case D3D11_DSV_DIMENSION_TEXTURE2DMS:count=1;break;
        case D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY:first=v.Texture2DMSArray.FirstArraySlice;count=v.Texture2DMSArray.ArraySize;break;
        default:break;
        }
        return std::format("{}{}_view format={} dimension={} mip={} first_slice={} array_size={} flags={}\n",name,slot,unsigned(v.Format),unsigned(v.ViewDimension),mip,first,count,v.Flags);
    }
    static std::string ViewDescription(const char* name,unsigned slot,ID3D11UnorderedAccessView* view) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC v{};view->GetDesc(&v);unsigned mip=0,first=0,count=0;
        if(v.ViewDimension==D3D11_UAV_DIMENSION_TEXTURE2D){mip=v.Texture2D.MipSlice;count=1;}
        else if(v.ViewDimension==D3D11_UAV_DIMENSION_TEXTURE2DARRAY){mip=v.Texture2DArray.MipSlice;first=v.Texture2DArray.FirstArraySlice;count=v.Texture2DArray.ArraySize;}
        return std::format("{}{}_view format={} dimension={} mip={} first_slice={} array_size={}\n",name,slot,unsigned(v.Format),unsigned(v.ViewDimension),mip,first,count);
    }
    void DepthStencil(ID3D11DeviceContext* context,ID3D11DepthStencilView* view) {
        const auto snapshot=DepthStencilDx11::Capture(context,view);const auto& d=snapshot.desc;
        ViewIdentity("ds",0,view);
        Microsoft::WRL::ComPtr<ID3D11Resource> resource;Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        D3D11_DEPTH_STENCIL_VIEW_DESC v{};D3D11_RESOURCE_DIMENSION dimension=D3D11_RESOURCE_DIMENSION_UNKNOWN;
        if(view){view->GetDesc(&v);view->GetResource(&resource);if(resource){resource->GetType(&dimension);resource.As(&texture);}}
        metadata+=std::format("dsv_bound={} dsv_valid={} dsv_texture2d={} dsv_resource={} dsv_resource_dimension={} dsv_format={} dsv_dimension={} dsv_flags={}\n",
            snapshot.bound,bool(resource),bool(texture),ResourceId(resource.Get()),unsigned(dimension),unsigned(v.Format),unsigned(v.ViewDimension),v.Flags);
        unsigned mip=0,first=0,count=0;
        if(v.ViewDimension==D3D11_DSV_DIMENSION_TEXTURE2D){mip=v.Texture2D.MipSlice;count=1;}
        else if(v.ViewDimension==D3D11_DSV_DIMENSION_TEXTURE2DARRAY){mip=v.Texture2DArray.MipSlice;first=v.Texture2DArray.FirstArraySlice;count=v.Texture2DArray.ArraySize;}
        else if(v.ViewDimension==D3D11_DSV_DIMENSION_TEXTURE2DMS)count=1;
        else if(v.ViewDimension==D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY){first=v.Texture2DMSArray.FirstArraySlice;count=v.Texture2DMSArray.ArraySize;}
        metadata+=std::format("dsv_mip={} dsv_first_slice={} dsv_array_size={} depth_state_default={} depth_enable={} depth_write_mask={} depth_func={} stencil_enable={} stencil_read_mask={} stencil_write_mask={} stencil_ref={} ds_coverage={}\n",
            mip,first,count,snapshot.defaultState,bool(d.DepthEnable),unsigned(d.DepthWriteMask),unsigned(d.DepthFunc),bool(d.StencilEnable),unsigned(d.StencilReadMask),unsigned(d.StencilWriteMask),snapshot.reference,snapshot.Label());
        const auto face=[&](const char* name,const D3D11_DEPTH_STENCILOP_DESC& f){metadata+=std::format("stencil_{} func={} fail_op={} depth_fail_op={} pass_op={}\n",name,unsigned(f.StencilFunc),unsigned(f.StencilFailOp),unsigned(f.StencilDepthFailOp),unsigned(f.StencilPassOp));};
        face("front",d.FrontFace);face("back",d.BackFace);
    }
    void RasterState(ID3D11DeviceContext* context) {
        UINT count=16;std::array<D3D11_VIEWPORT,16> viewports{};context->RSGetViewports(&count,viewports.data());
        metadata+=std::format("viewport_count={}\n",count);
        for(unsigned i=0;i<count;++i){const auto& v=viewports[i];metadata+=std::format("viewport{} x={} y={} width={} height={} min_z={} max_z={}\n",i,v.TopLeftX,v.TopLeftY,v.Width,v.Height,v.MinDepth,v.MaxDepth);}
        count=16;std::array<D3D11_RECT,16> rectangles{};context->RSGetScissorRects(&count,rectangles.data());
        metadata+=std::format("scissor_count={}\n",count);
        for(unsigned i=0;i<count;++i){const auto& r=rectangles[i];metadata+=std::format("scissor{} left={} top={} right={} bottom={}\n",i,r.left,r.top,r.right,r.bottom);}
        Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster;context->RSGetState(&raster);
        D3D11_RASTERIZER_DESC rs{};
        if(raster)raster->GetDesc(&rs);
        else {rs.FillMode=D3D11_FILL_SOLID;rs.CullMode=D3D11_CULL_BACK;rs.DepthClipEnable=TRUE;}
        metadata+=std::format("raster bound={} fill={} cull={} front_ccw={} depth_bias={} depth_bias_clamp={} slope_bias={} depth_clip={} scissor={} multisample={} aa_line={}\n",
            bool(raster),unsigned(rs.FillMode),unsigned(rs.CullMode),bool(rs.FrontCounterClockwise),rs.DepthBias,rs.DepthBiasClamp,rs.SlopeScaledDepthBias,bool(rs.DepthClipEnable),bool(rs.ScissorEnable),bool(rs.MultisampleEnable),bool(rs.AntialiasedLineEnable));
        Microsoft::WRL::ComPtr<ID3D11BlendState> blend;std::array<float,4> factor{};UINT mask=0;context->OMGetBlendState(&blend,factor.data(),&mask);
        D3D11_BLEND_DESC bs{};
        if(blend)blend->GetDesc(&bs);
        else for(auto& rt:bs.RenderTarget){rt.SrcBlend=rt.SrcBlendAlpha=D3D11_BLEND_ONE;rt.DestBlend=rt.DestBlendAlpha=D3D11_BLEND_ZERO;rt.BlendOp=rt.BlendOpAlpha=D3D11_BLEND_OP_ADD;rt.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;}
        metadata+=std::format("blend bound={} alpha_to_coverage={} independent={} sample_mask={} factor={},{},{},{}\n",bool(blend),bool(bs.AlphaToCoverageEnable),bool(bs.IndependentBlendEnable),mask,factor[0],factor[1],factor[2],factor[3]);
        for(unsigned i=0;i<8;++i){const auto& rt=bs.RenderTarget[i];metadata+=std::format("blend{} enabled={} src={} dst={} op={} src_alpha={} dst_alpha={} op_alpha={} write_mask={}\n",i,bool(rt.BlendEnable),unsigned(rt.SrcBlend),unsigned(rt.DestBlend),unsigned(rt.BlendOp),unsigned(rt.SrcBlendAlpha),unsigned(rt.DestBlendAlpha),unsigned(rt.BlendOpAlpha),unsigned(rt.RenderTargetWriteMask));}
        D3D11_PRIMITIVE_TOPOLOGY topology{};context->IAGetPrimitiveTopology(&topology);metadata+=std::format("topology={}\n",unsigned(topology));
        Microsoft::WRL::ComPtr<ID3D11GeometryShader> gs;Microsoft::WRL::ComPtr<ID3D11HullShader> hs;Microsoft::WRL::ComPtr<ID3D11DomainShader> ds;
        context->GSGetShader(&gs,nullptr,nullptr);context->HSGetShader(&hs,nullptr,nullptr);context->DSGetShader(&ds,nullptr,nullptr);
        metadata+=std::format("geometry_shader_bound={} hull_shader_bound={} domain_shader_bound={}\n",bool(gs),bool(hs),bool(ds));
    }
    void Samplers(ID3D11DeviceContext* context,Stage stage) {
        std::array<ID3D11SamplerState*,16> raw{};
        if(stage==Stage::Pixel)context->PSGetSamplers(0,UINT(raw.size()),raw.data());
        else context->CSGetSamplers(0,UINT(raw.size()),raw.data());
        for(unsigned i=0;i<raw.size();++i) {
            Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler;sampler.Attach(raw[i]);
            if(!sampler){metadata+=std::format("sampler{} bound=false\n",i);continue;}
            D3D11_SAMPLER_DESC d{};sampler->GetDesc(&d);
            metadata+=std::format("sampler{} bound=true filter={} address_u={} address_v={} address_w={} mip_bias={} anisotropy={} comparison={} border={},{},{},{} min_lod={} max_lod={}\n",
                i,unsigned(d.Filter),unsigned(d.AddressU),unsigned(d.AddressV),unsigned(d.AddressW),d.MipLODBias,d.MaxAnisotropy,unsigned(d.ComparisonFunc),d.BorderColor[0],d.BorderColor[1],d.BorderColor[2],d.BorderColor[3],d.MinLOD,d.MaxLOD);
        }
    }
    void RoleBindings(ID3D11DeviceContext* context,Stage stage,const analysis::ObservationContract& observation) {
        for(const auto& role:observation.roles) {
            if(role.slot>=inputs.size())continue;
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
            if(stage==Stage::Pixel)context->PSGetShaderResources(role.slot,1,&view);
            else context->CSGetShaderResources(role.slot,1,&view);
            inputs[role.slot]=ViewIdentity("t",role.slot,view.Get());
            if(view){inputs[role.slot].format=inputs[role.slot].span.format;inputViews[role.slot]=ViewDescription("t",role.slot,view.Get());}
        }
    }
    void InputBindings(ID3D11DeviceContext* context,Stage stage) {
        Samplers(context,stage);
        std::array<ID3D11ShaderResourceView*,128> raw{};
        if(stage==Stage::Pixel)context->PSGetShaderResources(0,UINT(raw.size()),raw.data());
        else context->CSGetShaderResources(0,UINT(raw.size()),raw.data());
        for(unsigned i=0;i<raw.size();++i) {
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;view.Attach(raw[i]);inputs[i]=ViewIdentity("t",i,view.Get());
            if(view){D3D11_SHADER_RESOURCE_VIEW_DESC v{};view->GetDesc(&v);inputs[i].format=v.Format;inputViews[i]=ViewDescription("t",i,view.Get());}
        }
    }
    void Bindings(ID3D11DeviceContext* context,Stage stage) {
        InputBindings(context,stage);
        if(stage==Stage::Pixel) {
            std::array<ID3D11RenderTargetView*,8> outputs{};Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depth;
            context->OMGetRenderTargets(UINT(outputs.size()),outputs.data(),&depth);
            for(unsigned i=0;i<outputs.size();++i) {
                Microsoft::WRL::ComPtr<ID3D11RenderTargetView> view;view.Attach(outputs[i]);ViewIdentity("rt",i,view.Get());
            }
            DepthStencil(context,depth.Get());
            RasterState(context);
        } else {
            std::array<ID3D11UnorderedAccessView*,8> outputs{};context->CSGetUnorderedAccessViews(0,UINT(outputs.size()),outputs.data());
            for(unsigned i=0;i<outputs.size();++i) {
                Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> view;view.Attach(outputs[i]);ViewIdentity("u",i,view.Get());
            }
        }
    }
};

struct RasterTarget {
    uint64_t resource=0,sourceFrame=0;
    CandidateKey candidate{};
    const char* role="";
    std::string view;
    unsigned slot=0;
    ViewSpan span{};
    analysis::CapturePolicy capture=analysis::CapturePolicy::Consecutive;
};
struct RasterWatch {
    ID3D11DeviceContext* context=nullptr; // identity only; DrawObservation owns the callback's context
    uint64_t revision=0;
    std::vector<RasterTarget> targets;
    uint64_t startedMs=0;
    unsigned captureEpoch=0;
};
struct CaptureIdentity {
    uint64_t revision=0,startedMs=0;
    unsigned epoch=0,burst=0;
    bool operator==(const CaptureIdentity&) const=default;
    static CaptureIdentity From(const RasterWatch& watch,uint64_t now) {
        CaptureWindow window;window.startedMs=watch.startedMs;window.epoch=watch.captureEpoch;
        return {watch.revision,watch.startedMs,watch.captureEpoch,window.Burst(now)};
    }
};
struct OwnerIdentity {
    uintptr_t context=0;DWORD thread=0;uint64_t frame=0,epoch=0;
    bool operator==(const OwnerIdentity&) const=default;
    static OwnerIdentity From(const DrawObservationDx11::Owner& owner) {
        return {reinterpret_cast<uintptr_t>(owner.context.Get()),owner.thread,owner.frame,owner.epoch};
    }
};
struct RasterMatch { RasterTarget target;unsigned outputSlot=0;ViewSpan output{}; };
inline std::atomic<uint64_t> rasterRecordSerial{0};
struct RasterReceipt {
    CaptureIdentity capture;OwnerIdentity owner;
    uint64_t cookie=0,recordId=0;
    std::vector<RasterMatch> matches;
};
// Immediate-context callbacks and resolve admission share the owner thread.
// Never acquire Registry from an observer: it can be held across original Draw.
class RasterMailbox {
    std::vector<RasterReceipt> rows_;
public:
    void Reset() {rows_.clear();}
    void Publish(RasterReceipt receipt) {
        if(!rows_.empty()&&(rows_.back().owner!=receipt.owner||rows_.back().capture!=receipt.capture))rows_.clear();
        if(rows_.size()==8)rows_.erase(rows_.begin());
        rows_.push_back(std::move(receipt));
    }
    std::string Join(CaptureIdentity capture,OwnerIdentity owner,CandidateKey candidate,const Frame& current) const {
        std::string result;
        for(const auto& receipt:rows_) {
            if(receipt.capture!=capture||receipt.owner!=owner)continue;
            for(const auto& match:receipt.matches) {
                const auto& target=match.target;if(target.candidate!=candidate||target.slot>=current.inputs.size())continue;
                const auto& input=current.inputs[target.slot];
                if(input.resource!=target.resource||!input.span.Known()||input.span!=target.span||!input.span.Intersects(match.output))continue;
                result+=std::format("joined_raster_record={} cookie={} role={} input_slot={} resource={} output_slot={} output_mip={} output_first_slice={} output_slices={} authority=diagnostic_only\n",
                    receipt.recordId,receipt.cookie,target.role,target.slot,target.resource,match.outputSlot,match.output.mip,match.output.firstSlice,match.output.slices);
            }
        }
        return result;
    }
};
inline std::vector<RasterTarget> Targets(const Frame& frame,const analysis::Result& analyzed,CandidateKey candidate,uint64_t sourceFrame) {
    std::vector<RasterTarget> result;
    if(analyzed.observation)for(const auto& role:analyzed.observation->roles) {
        if(role.slot>=frame.inputs.size())continue;
        const auto& input=frame.inputs[role.slot];
        if(input.resource)result.push_back({input.resource,sourceFrame,candidate,role.name,frame.inputViews[role.slot],role.slot,input.span,analyzed.observation->capture});
    }
    for(const auto& recipe:analyzed.recipes) {
        const auto& color=frame.inputs[recipe.profile.colorSlot];
        const auto& motion=frame.inputs[recipe.profile.motionSlot];
        const auto& history=frame.inputs[recipe.historySlot];
        if(!color.resource||!motion.resource||!history.resource||!automatic::Color(color.format)||!automatic::Color(history.format)||
            color.width!=motion.width||color.height!=motion.height)continue;
        const Frame::TextureBinding* depth=nullptr;bool ambiguous=false;
        for(const auto slot:analyzed.textures) {
            const auto& input=frame.inputs[slot];
            if(!(input.bindFlags&D3D11_BIND_DEPTH_STENCIL)||input.width!=color.width||input.height!=color.height||
                (input.format!=DXGI_FORMAT_R32_FLOAT&&input.format!=DXGI_FORMAT_R16_UNORM&&
                 input.format!=DXGI_FORMAT_R24_UNORM_X8_TYPELESS&&input.format!=DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS))continue;
            if(depth&&depth->resource!=input.resource)ambiguous=true;else depth=&input;
        }
        if(!depth||ambiguous)continue;
        result.push_back({color.resource,sourceFrame,candidate,"color",frame.inputViews[recipe.profile.colorSlot],recipe.profile.colorSlot,color.span});
        result.push_back({motion.resource,sourceFrame,candidate,"motion",frame.inputViews[recipe.profile.motionSlot],recipe.profile.motionSlot,motion.span});
        result.push_back({depth->resource,sourceFrame,candidate,"depth",frame.inputViews[size_t(depth-frame.inputs.data())],unsigned(depth-frame.inputs.data()),depth->span});
    }
    return result;
}

// A diagnostic window shares the existing draw observer's ownership. It never
// participates in SR eligibility, resource retention, or GPU retirement.
class RasterWindow {
    struct Track { SampleBudget budget;unsigned priority=0;bool active=false; };
    struct Pending {
        Frame frame;
        std::pair<uint64_t,uint64_t> shaders;
        uint64_t cookie=0;
        RasterReceipt receipt;
    };
    ID3D11DeviceContext* context_=nullptr;
    uint64_t revision_=0,serial_=0,frame_=~uint64_t(0),ordinal_=0;
    size_t bytes_=0;
    struct Counters { uint64_t seen=0,owner=0,expired=0,target=0,shader=0,pairs=0,budget=0,unknown=0,queued=0,rejected=0,completion=0,bytes=0; } stats_;
    uint64_t reportedFrame_=~uint64_t(0);
    CaptureWindow window_;
    unsigned burst_=~0u,captureEpoch_=0;
    std::map<std::pair<uint64_t,uint64_t>,Track> tracks_;
    std::optional<Pending> pending_;
public:
    void Reset() {context_=nullptr;revision_=0;tracks_.clear();pending_.reset();bytes_=0;window_={};burst_=~0u;captureEpoch_=0;stats_={};reportedFrame_=~uint64_t(0);}
    bool Full() const {return bytes_>=8*1024*1024;}
    std::optional<std::string> Report(uint64_t frame) {
        if(reportedFrame_!=~uint64_t(0)&&frame>=reportedFrame_&&frame-reportedFrame_<60)return {};
        reportedFrame_=frame;
        return std::format("epoch={} seen={} owner_rejected={} expired={} target_miss={} shader_missing={} pair_budget={} sample_budget={} unknown={} queued={} queue_rejected={} completion_rejected={} byte_budget={}",
            captureEpoch_,stats_.seen,stats_.owner,stats_.expired,stats_.target,stats_.shader,stats_.pairs,stats_.budget,stats_.unknown,stats_.queued,stats_.rejected,stats_.completion,stats_.bytes);
    }
    template<class Export> uint64_t Before(const RasterWatch& watch,ID3D11DeviceContext* context,const DrawObservationDx11::Draw& draw,
        const DrawObservationDx11::Owner& owner,DrawObservationDx11::CapturedState& state,Export&& exportShader,uint64_t now=GetTickCount64()) {
        if(pending_||watch.context!=context||!owner.Matches(context)){++stats_.owner;return 0;}
        if(context_!=context||revision_!=watch.revision){Reset();context_=context;revision_=watch.revision;frame_=~uint64_t(0);window_.startedMs=watch.startedMs;}
        if(captureEpoch_!=watch.captureEpoch) {
            // Keep session byte usage across diagnostic-only extent changes.
            captureEpoch_=watch.captureEpoch;tracks_.clear();burst_=~0u;
            window_={};window_.startedMs=watch.startedMs;window_.epoch=captureEpoch_;
        }
        ++stats_.seen;
        if(Full()){++stats_.bytes;return 0;}
        const auto burst=window_.Active(now);if(!burst){++stats_.expired;return 0;}
        // New scene shader pairs get their own bounded opportunities in each
        // later burst rather than inheriting the loading scene's eight slots.
        if(burst_!=*burst){tracks_.clear();burst_=*burst;}
        if(frame_!=owner.frame){frame_=owner.frame;ordinal_=0;}
        const auto ordinal=++ordinal_;
        DrawObservationDx11::InternalScope internal;
        std::array<ID3D11RenderTargetView*,8> raw{};Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depth;
        context->OMGetRenderTargets(UINT(raw.size()),raw.data(),&depth);
        std::array<Microsoft::WRL::ComPtr<ID3D11RenderTargetView>,8> outputs;
        std::array<uint64_t,9> ids{};
        for(unsigned i=0;i<raw.size();++i){outputs[i].Attach(raw[i]);if(raw[i]){Microsoft::WRL::ComPtr<ID3D11Resource> resource;raw[i]->GetResource(&resource);ids[i]=ResourceId(resource.Get());}}
        if(depth){Microsoft::WRL::ComPtr<ID3D11Resource> resource;depth->GetResource(&resource);ids[8]=ResourceId(resource.Get());}
        if(std::none_of(watch.targets.begin(),watch.targets.end(),[&](const auto& target){return target.resource&&std::find(ids.begin(),ids.end(),target.resource)!=ids.end();})) {++stats_.target;return 0;}
        const auto pixel=state.PixelIdentity(),vertex=state.VertexIdentity();
        if(!pixel.hash||!vertex.hash){++stats_.shader;return 0;}
        const auto program=automatic::Find(state.PixelShader());
        const analysis::Result* declared=program?&program->Analyze(Stage::Pixel,pixel.hash):nullptr;
        Microsoft::WRL::ComPtr<ID3D11BlendState> blend;context->OMGetBlendState(&blend,nullptr,nullptr);
        D3D11_BLEND_DESC bs{};if(blend)blend->GetDesc(&bs);
        std::vector<RasterMatch> matches;unsigned priority=0;
        for(const auto& target:watch.targets)for(unsigned i=0;i<ids.size();++i) {
            if(!target.resource||ids[i]!=target.resource)continue;
            const unsigned mask=blend?bs.RenderTarget[bs.IndependentBlendEnable?i%8:0].RenderTargetWriteMask:D3D11_COLOR_WRITE_ENABLE_ALL;
            if(i<8&&(!mask||(declared&&declared->pixelOutputsKnown&&!(declared->pixelOutputMasks[i]&mask))))continue;
            const auto span=i<8?Subresources(outputs[i].Get()):Subresources(depth.Get());
            // Unknown views remain explicitly resource-only diagnostics, but
            // can never produce a subresource-qualified joined sample.
            if(target.span.Known()&&span.Known()&&!target.span.Intersects(span))continue;
            matches.push_back({target,i,span});
            priority=(std::max)(priority,(std::string_view(target.role)=="motion"||std::string_view(target.role)=="motion_hint")?2u:1u);
        }
        if(!priority){++stats_.target;return 0;}
        const auto key=std::pair{pixel.hash,vertex.hash};auto track=tracks_.find(key);
        if(track!=tracks_.end()&&!track->second.budget.CanBegin(owner.frame)){++stats_.budget;return 0;}
        if(track==tracks_.end()||!track->second.active){
            if(std::count_if(tracks_.begin(),tracks_.end(),[](const auto& entry){return entry.second.active;})>=8) {
                const auto victim=std::find_if(tracks_.begin(),tracks_.end(),[&](const auto& entry){return entry.second.active&&entry.second.priority<priority;});
                if(victim==tracks_.end()){++stats_.pairs;return 0;}
                // Retain the attempted-frame ledger when a motion pair takes
                // a lower-priority slot. Rebinding cannot replay that frame.
                victim->second.active=false;
            }
            if(track==tracks_.end())track=tracks_.emplace(key,Track{}).first;
            track->second.active=true;
        }
        track->second.priority=(std::max)(track->second.priority,priority);
        if(!track->second.budget.Begin(owner.frame)){++stats_.budget;return 0;}
        Pending next;next.shaders=key;next.cookie=++serial_;
        next.frame.recordId=++rasterRecordSerial;
        next.receipt={CaptureIdentity::From(watch,now),OwnerIdentity::From(owner),next.cookie,next.frame.recordId,std::move(matches)};
        next.frame.metadata=std::format("schema=1\npid={}\nkind=raster_binding_match\nowner_frame={}\nowner_epoch={}\ndraw_ordinal={}\ndraw_kind={}\ncount={}\nstart={}\nbase={}\ninstances={}\ninstance_start={}\ncount_known={}\nindirect_offset={}\npixel_shader={:016X}\nvertex_shader={:016X}\nconstant_source=observed_cpu_writes_only\n",
            GetCurrentProcessId(),owner.frame,owner.epoch,ordinal,unsigned(draw.kind),draw.count,draw.start,draw.base,draw.instances,draw.instanceStart,draw.countKnown,draw.indirectOffset,pixel.hash,vertex.hash);
        next.frame.metadata+=std::format("capture_burst={} capture_elapsed_ms={} capture_epoch={}\n",*burst,now-window_.startedMs,captureEpoch_);
        next.frame.metadata+=std::format("capture_reason={} config_revision={} context_id={} thread_id={} replacement_authority=false\n",
            captureEpoch_?"input-extent":"initial",watch.revision,reinterpret_cast<uintptr_t>(context),GetCurrentThreadId());
        next.frame.metadata+=std::format("raster_record={} cookie={}\n",next.frame.recordId,next.cookie);
        for(const auto& match:next.receipt.matches) {
            const auto& target=match.target;
            next.frame.metadata+=std::format("target candidate={} discovery_source_frame={} role={} resource={} input_slot={} output_slot={} match=possible_output_binding\n{}",
                KeyText(target.candidate),target.sourceFrame,target.role,target.resource,target.slot,match.outputSlot,target.view);
        }
        for(unsigned i=0;i<outputs.size();++i)next.frame.ViewIdentity("rt",i,outputs[i].Get());
        next.frame.DepthStencil(context,depth.Get());
        next.frame.RasterState(context);
        next.frame.InputBindings(context,Stage::Pixel);
        const auto constants=[&](ID3D11DeviceChild* shader,ShaderStage stage,uint64_t hash) {
            if(const auto program=automatic::Find(shader)) {
                const auto& analyzed=program->Analyze(Stage::Pixel,hash,[&](auto&& bytes,auto&& assembly,const analysis::Result& result){return exportShader(stage,hash,std::move(bytes),std::move(assembly),result);},{watch.revision,*burst});
                next.frame.Constants(context,stage,analyzed);
            }
        };
        constants(state.PixelShader(),ShaderStage::Pixel,pixel.hash);constants(state.VertexShader(),ShaderStage::Vertex,vertex.hash);
        const bool known=next.frame.knownBytes!=0;
        if(!known)++stats_.unknown;
        const bool joinTarget=std::any_of(next.receipt.matches.begin(),next.receipt.matches.end(),[](const auto& match){return match.target.capture==analysis::CapturePolicy::SeedThenJoined;});
        if(!track->second.budget.Wants(known,joinTarget))return 0;
        if(next.frame.buffers.size()+next.frame.metadata.size()>8*1024*1024-bytes_){++stats_.bytes;return 0;}
        pending_=std::move(next);return pending_->cookie;
    }
    template<class Sink> std::optional<RasterReceipt> After(uint64_t cookie,DrawObservationDx11::Completion completion,
        const RasterWatch& current,const DrawObservationDx11::Owner& owner,Sink&& sink,uint64_t now=GetTickCount64()) {
        if(!cookie||!pending_||pending_->cookie!=cookie)return {};
        auto next=std::move(*pending_);pending_.reset();
        if(completion!=DrawObservationDx11::Completion::OriginalReturned||current.context!=context_||!owner.Matches(context_)||
            next.receipt.capture!=CaptureIdentity::From(current,now)||next.receipt.owner!=OwnerIdentity::From(owner)) {++stats_.completion;return {};}
        next.frame.metadata+="completion=original_returned\nwriter_authority=binding_only_not_pixel_or_gpu_completion_proof\n";
        const auto size=next.frame.buffers.size()+next.frame.metadata.size();
        if(size>8*1024*1024-bytes_){++stats_.bytes;return {};}
        const bool known=next.frame.knownBytes!=0;
        if(sink(next.shaders.first,next.shaders.second,std::move(next.frame))) {
            tracks_.at(next.shaders).budget.Queued(known);bytes_+=size;++stats_.queued;
            return std::move(next.receipt);
        }
        ++stats_.rejected;return {};
    }
};
} // namespace taa::evidence
