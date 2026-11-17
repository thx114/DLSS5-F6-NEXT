// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "TaaProfile.h"
#include "TaaConstantBuffer.h"
#include "TaaProjectionJitterDx11.h"
#include "TaaDepthStencilDx11.h"
#include <d3d11_1.h>
#include <runtime/Dx11ContextState.h>
#include <d3dcompiler.h>
#include <array>
#include <functional>

namespace taa {
using Microsoft::WRL::ComPtr;
struct Invocation {
    Stage stage = Stage::Pixel;
    unsigned count = 0, instances = 1;
    unsigned groupsX = 0, groupsY = 0, groupsZ = 0;
    bool direct = true;
};
struct Frame {
    ID3D11Texture2D *color = nullptr, *depth = nullptr, *motion = nullptr, *output = nullptr;
    unsigned renderWidth = 0, renderHeight = 0, outputWidth = 0, outputHeight = 0;
    float jitterX = 0, jitterY = 0;
    bool reset = true;
    ID3D11Texture2D* sourceColor = nullptr;
    DXGI_FORMAT colorStorage = DXGI_FORMAT_UNKNOWN, colorView = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT sourceStorage = DXGI_FORMAT_UNKNOWN, sourceView = DXGI_FORMAT_UNKNOWN;
};
using dx11::ContextStateGuard;
inline constexpr char GuideShader[] = R"(
Texture2D<float> sourceDepth : register(t0);
Texture2D<float2> sourceMotion : register(t1);
RWTexture2D<float> depth : register(u0);
RWTexture2D<float2> motion : register(u1);
cbuffer Settings : register(b0) { uint width, height, encoding, sampling; float2 scale; float2 padding; };
[numthreads(8,8,1)] void main(uint3 p : SV_DispatchThreadID) {
    if (p.x >= width || p.y >= height) return;
    int2 location = int2(p.xy);
    if (sampling == 1) {
        float nearest = sourceDepth.Load(int3(location,0));
        const int2 offsets[4] = {int2(-1,-1),int2(1,-1),int2(-1,1),int2(1,1)};
        [unroll] for (uint i=0;i<4;++i) {
            int2 candidate = int2(p.xy) + offsets[i];
            float z = sourceDepth.Load(int3(candidate,0));
            if (z >= nearest) { nearest=z; location=candidate; }
        }
    }
    float2 v = sourceMotion.Load(int3(location,0));
    if (encoding == 1) { float2 d = v - (127.0 / 255.0); v = sign(d) * (2*d) * (2*d); }
    depth[p.xy] = sourceDepth.Load(int3(p.xy,0));
    motion[p.xy] = v * scale;
})";

// API-only transaction, also executed by the WARP fixture. Begin freezes inputs
// BEFORE the original TAA command. Finish runs AFTER it, preserving all of the
// original pass's other MRT/UAV/history writes. The caller provides the one
// existing SR pipeline, and only a proven current output authorizes copyback.
class PassDx11 final {
    struct Input {
        ComPtr<ID3D11Texture2D> source, snapshot;
        ComPtr<ID3D11ShaderResourceView> view;
        D3D11_TEXTURE2D_DESC desc{};
        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
    };
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11DeviceContext1> context1_;
    ComPtr<ID3DDeviceContextState> workState_;
    ComPtr<ID3D11Query> done_;
    ComPtr<ID3D11ComputeShader> convert_;
    ComPtr<ID3D11Buffer> settings_;
    ComPtr<ID3D11Texture2D> depth_, motion_, output_, target_;
    ComPtr<ID3D11UnorderedAccessView> depthUav_, motionUav_;
    std::array<Input,3> input_;
    D3D11_TEXTURE2D_DESC targetDesc_{};
    DXGI_FORMAT targetFormat_ = DXGI_FORMAT_UNKNOWN;
    bool recording_ = false, submitted_ = false;
    bool allocated_ = false;
    std::array<D3D11_TEXTURE2D_DESC,3> madeInputs_{};
    std::array<DXGI_FORMAT,3> madeFormats_{};
    D3D11_TEXTURE2D_DESC madeTarget_{};
    DXGI_FORMAT madeTargetFormat_ = DXGI_FORMAT_UNKNOWN;
    Profile profile_;
    Frame frame_;
    std::string error_;

    bool Fail(const char* reason) { error_ = reason; return false; }
    bool Texture(ComPtr<ID3D11Texture2D>& out, unsigned width, unsigned height, DXGI_FORMAT format, unsigned bind) {
        D3D11_TEXTURE2D_DESC d{}; d.Width=width; d.Height=height; d.Format=format;
        d.MipLevels=d.ArraySize=d.SampleDesc.Count=1; d.BindFlags=bind;
        return SUCCEEDED(device_->CreateTexture2D(&d,nullptr,&out));
    }
    bool ReadInput(unsigned slot, Input& out) {
        ComPtr<ID3D11ShaderResourceView> view;
        if (profile_.stage == Stage::Pixel) context_->PSGetShaderResources(slot,1,&view);
        else context_->CSGetShaderResources(slot,1,&view);
        if (!view) return Fail("missing input SRV");
        view->GetDesc(&out.srv);
        ComPtr<ID3D11Resource> resource; view->GetResource(&resource);
        if (FAILED(resource.As(&out.source))) return Fail("input is not a 2D texture");
        out.source->GetDesc(&out.desc);
        if (out.srv.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D || out.srv.Texture2D.MostDetailedMip ||
            out.desc.MipLevels!=1 || out.desc.ArraySize!=1 || out.desc.SampleDesc.Count!=1)
            return Fail("only full, single-sample, mip-0 2D inputs are supported");
        return true;
    }
    bool ValidateFormats() {
        const auto color=input_[0].srv.Format,depth=input_[1].srv.Format,motion=input_[2].srv.Format;
        const bool colorFloat=color==DXGI_FORMAT_R16G16B16A16_FLOAT || color==DXGI_FORMAT_R32G32B32A32_FLOAT ||
            color==DXGI_FORMAT_R11G11B10_FLOAT || color==DXGI_FORMAT_R10G10B10A2_UNORM || color==DXGI_FORMAT_R8G8B8A8_UNORM ||
            color==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || color==DXGI_FORMAT_B8G8R8A8_UNORM || color==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        const bool depthFloat=depth==DXGI_FORMAT_R32_FLOAT || depth==DXGI_FORMAT_R16_UNORM ||
            depth==DXGI_FORMAT_R24_UNORM_X8_TYPELESS || depth==DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        const bool motionFloat=motion==DXGI_FORMAT_R10G10B10A2_UNORM || motion==DXGI_FORMAT_R32G32_FLOAT || motion==DXGI_FORMAT_R16G16_FLOAT ||
            motion==DXGI_FORMAT_R16G16_UNORM || motion==DXGI_FORMAT_R16G16_SNORM || motion==DXGI_FORMAT_R8G8B8A8_UNORM ||
            motion==DXGI_FORMAT_R16G16B16A16_FLOAT || motion==DXGI_FORMAT_R32G32B32A32_FLOAT || motion==DXGI_FORMAT_R16G16B16A16_UNORM;
        return (colorFloat && depthFloat && motionFloat) || Fail("unsupported typed color/depth/motion format");
    }
    bool ReadTarget(const Invocation& call) {
        if(profile_.outputSlot>=8)return Fail("TAA output slot is out of range");
        ComPtr<ID3D11Resource> resource;
        ComPtr<ID3D11DepthStencilView> depthView;
        if (profile_.stage == Stage::Pixel) {
            std::array<ComPtr<ID3D11RenderTargetView>,8> held;
            std::array<ID3D11RenderTargetView*,8> views{};
            context_->OMGetRenderTargets(8,views.data(),&depthView);
            for (unsigned i=0;i<views.size();++i) held[i].Attach(views[i]);
            const auto& view=held[profile_.outputSlot];
            if (!view) return Fail("missing output RTV");
            D3D11_RENDER_TARGET_VIEW_DESC d{}; view->GetDesc(&d);
            if (d.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2D || d.Texture2D.MipSlice) return Fail("unsupported output RTV subresource");
            targetFormat_=d.Format; view->GetResource(&resource);
        } else {
            ComPtr<ID3D11UnorderedAccessView> view; context_->CSGetUnorderedAccessViews(profile_.outputSlot,1,&view);
            if (!view) return Fail("missing output UAV");
            D3D11_UNORDERED_ACCESS_VIEW_DESC d{}; view->GetDesc(&d);
            if (d.ViewDimension!=D3D11_UAV_DIMENSION_TEXTURE2D || d.Texture2D.MipSlice) return Fail("unsupported output UAV subresource");
            targetFormat_=d.Format; view->GetResource(&resource);
        }
        if (FAILED(resource.As(&target_))) return Fail("output is not a 2D texture");
        target_->GetDesc(&targetDesc_);
        if (targetDesc_.MipLevels!=1 || targetDesc_.ArraySize!=1 || targetDesc_.SampleDesc.Count!=1)
            return Fail("unsupported output texture layout");
        if (!call.direct) return Fail("indirect TAA commands are observation-only");
        ComPtr<ID3D11Predicate> predicate; BOOL predicateValue=FALSE; context_->GetPredication(&predicate,&predicateValue);
        if (predicate) return Fail("predicated TAA command");
        if (profile_.stage == Stage::Compute) {
            if (call.groupsZ!=1 || call.groupsX!=(targetDesc_.Width+profile_.threadsX-1)/profile_.threadsX ||
                call.groupsY!=(targetDesc_.Height+profile_.threadsY-1)/profile_.threadsY) return Fail("partial or mismatched TAA dispatch");
        } else {
            if (call.instances!=1 || (call.count!=3 && call.count!=4 && call.count!=6)) return Fail("unsupported fullscreen TAA draw");
            D3D11_PRIMITIVE_TOPOLOGY topology{}; context_->IAGetPrimitiveTopology(&topology);
            if (topology!=D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST && topology!=D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP) return Fail("unsupported TAA topology");
            std::array<D3D11_VIEWPORT,16> viewports{}; UINT count=UINT(viewports.size()); context_->RSGetViewports(&count,viewports.data());
            if (count!=1 || viewports[0].TopLeftX!=0 || viewports[0].TopLeftY!=0 ||
                viewports[0].Width!=float(targetDesc_.Width) || viewports[0].Height!=float(targetDesc_.Height)) return Fail("partial TAA viewport");
            ComPtr<ID3D11RasterizerState> raster; context_->RSGetState(&raster);
            if (raster) { D3D11_RASTERIZER_DESC d{}; raster->GetDesc(&d);
                if (d.ScissorEnable) { std::array<D3D11_RECT,16> rects{}; count=UINT(rects.size()); context_->RSGetScissorRects(&count,rects.data());
                    if (count!=1 || rects[0].left>0 || rects[0].top>0 || rects[0].right<LONG(targetDesc_.Width) || rects[0].bottom<LONG(targetDesc_.Height)) return Fail("partial TAA scissor"); }
            }
            // The original draw still owns every depth/stencil side effect.
            // Full color copyback is valid only when DS cannot discard pixels.
            const auto depth=DepthStencilDx11::Capture(context_.Get(),depthView.Get());
            if (const auto* reason=depth.Reason()) return Fail(reason);
            ComPtr<ID3D11BlendState> blend; FLOAT factor[4]{}; UINT mask=0; context_->OMGetBlendState(&blend,factor,&mask);
            if (!(mask&1)) return Fail("TAA sample mask disables the draw");
            if (blend) { D3D11_BLEND_DESC d{}; blend->GetDesc(&d); const auto& rt=d.RenderTarget[d.IndependentBlendEnable?profile_.outputSlot:0];
                if (rt.BlendEnable || rt.RenderTargetWriteMask!=D3D11_COLOR_WRITE_ENABLE_ALL) return Fail("blended or masked TAA output"); }
            ComPtr<ID3D11GeometryShader> gs; ComPtr<ID3D11HullShader> hs; ComPtr<ID3D11DomainShader> dsShader;
            context_->GSGetShader(&gs,nullptr,nullptr); context_->HSGetShader(&hs,nullptr,nullptr); context_->DSGetShader(&dsShader,nullptr,nullptr);
            if (gs || hs || dsShader) return Fail("unsupported fullscreen TAA geometry stages");
            std::array<ID3D11Buffer*,4> so{};context_->SOGetTargets(UINT(so.size()),so.data());
            bool streamOutput=false;for(auto* buffer:so)if(buffer){streamOutput=true;buffer->Release();}
            if (streamOutput) return Fail("stream-output TAA draw");
        }
        return true;
    }
    bool ReadJitter(jitter::Input jitter) {
        if(profile_.jitterSource==JitterSource::MotionProjection) {
            if(!jitter.sample||!jitter.sample->Matches(context_.Get(),jitter.revision,input_[2].source.Get(),input_[1].source.Get(),frame_.renderWidth,frame_.renderHeight,input_[2].srv.Format))
                return Fail("waiting for same-frame raster projection jitter");
            frame_.jitterX=jitter.sample->offset.x;frame_.jitterY=jitter.sample->offset.y;return true;
        }
        ComPtr<ID3D11Buffer> buffer; UINT first=0,count=0;
        if (profile_.stage==Stage::Pixel) context1_->PSGetConstantBuffers1(profile_.jitterSlot,1,&buffer,&first,&count);
        else context1_->CSGetConstantBuffers1(profile_.jitterSlot,1,&buffer,&first,&count);
        if (!buffer || profile_.jitterOffsetX/16>=count || profile_.jitterOffsetY/16>=count) return Fail("jitter outside bound CB1 range");
        float x=0,y=0;
        const bool haveX=constants::Read(buffer.Get(),first*16+profile_.jitterOffsetX,x);
        const bool haveY=constants::Read(buffer.Get(),first*16+profile_.jitterOffsetY,y);
        if (!haveX || !haveY) return Fail("waiting for an observed write of the selected jitter CB");
        x*=PixelScale(profile_.jitterUnits,frame_.renderWidth,profile_.jitterScaleX);
        y*=PixelScale(profile_.jitterUnits,frame_.renderHeight,profile_.jitterScaleY);
        if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x)>float(frame_.renderWidth) || std::abs(y)>float(frame_.renderHeight)) return Fail("invalid current jitter");
        frame_.jitterX=x; frame_.jitterY=y; return true;
    }
    bool Allocate() {
        for (auto& input : input_) {
            auto d=input.desc; d.BindFlags=D3D11_BIND_SHADER_RESOURCE; d.Usage=D3D11_USAGE_DEFAULT; d.CPUAccessFlags=d.MiscFlags=0;
            if (&input==&input_[0]) d.Format=input.srv.Format; // NGX receives a typed color resource
            if (FAILED(device_->CreateTexture2D(&d,nullptr,&input.snapshot)) ||
                FAILED(device_->CreateShaderResourceView(input.snapshot.Get(),&input.srv,&input.view))) return Fail("input snapshot allocation failed");
        }
        if (!Texture(depth_,frame_.renderWidth,frame_.renderHeight,DXGI_FORMAT_R32_FLOAT,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS) ||
            !Texture(motion_,frame_.renderWidth,frame_.renderHeight,DXGI_FORMAT_R32G32_FLOAT,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS) ||
            FAILED(device_->CreateUnorderedAccessView(depth_.Get(),nullptr,&depthUav_)) || FAILED(device_->CreateUnorderedAccessView(motion_.Get(),nullptr,&motionUav_))) return Fail("guide allocation failed");
        auto outputFormat=targetFormat_;
        if (outputFormat==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) outputFormat=DXGI_FORMAT_R8G8B8A8_UNORM;
        if (outputFormat==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) outputFormat=DXGI_FORMAT_B8G8R8A8_UNORM;
        UINT support=0;
        if (FAILED(device_->CheckFormatSupport(outputFormat,&support)) || !(support&D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW)) return Fail("output format has no typed UAV support");
        if (!Texture(output_,frame_.outputWidth,frame_.outputHeight,outputFormat,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS)) return Fail("output allocation failed");
        for (unsigned i=0;i<3;++i) {madeInputs_[i]=input_[i].desc;madeFormats_[i]=input_[i].srv.Format;}
        madeTarget_=targetDesc_;madeTargetFormat_=targetFormat_;allocated_=true;
        return true;
    }
    void Seal() { if (recording_) { context_->End(done_.Get()); recording_=false; submitted_=true; } }
public:
    PassDx11(ID3D11Device* device, ID3D11DeviceContext* context) : device_(device),context_(context) {}
    const std::string& Error() const { return error_; }
    bool Idle() {
        if (recording_) return false;
        if (!submitted_) return true;
        BOOL completed=FALSE;
        if (context_->GetData(done_.Get(),&completed,sizeof(completed),D3D11_ASYNC_GETDATA_DONOTFLUSH)!=S_OK || !completed) return false;
        submitted_=false; return true;
    }
    // Discovery and Begin use the same checks. Probe records no GPU commands.
    bool Probe(const Profile& profile, const Invocation& call,jitter::Input jitter={}) {
        profile_=profile; frame_={};
        if (call.stage!=profile.stage) return Fail("TAA stage mismatch");
        if (context_->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE || FAILED(context_.As(&context1_))) return Fail("TAA requires a D3D11.1 immediate context");
        if (!ReadTarget(call) || !ReadInput(profile.colorSlot,input_[0]) || !ReadInput(profile.depthSlot,input_[1]) || !ReadInput(profile.motionSlot,input_[2]) || !ValidateFormats()) return false;
        frame_.renderWidth=input_[0].desc.Width; frame_.renderHeight=input_[0].desc.Height;
        frame_.outputWidth=targetDesc_.Width; frame_.outputHeight=targetDesc_.Height;
        if (!frame_.renderWidth || !frame_.renderHeight || frame_.outputWidth<frame_.renderWidth || frame_.outputHeight<frame_.renderHeight) return Fail("unsupported render/output extents");
        for (unsigned i=1;i<3;++i) if (input_[i].desc.Width!=frame_.renderWidth || input_[i].desc.Height!=frame_.renderHeight) return Fail("guides must match render resolution");
        if (!ReadJitter(jitter)) return false;
        return true;
    }
    bool Begin(const Profile& profile, const Invocation& call, bool reset,jitter::Input jitter={}) {
        if (!Idle()) return Fail("TAA input still in GPU use");
        // Replace source references only after the slot's actual event retires.
        if (!Probe(profile,call,jitter)) return false;
        frame_.reset=reset;
        if (!workState_) {
            ComPtr<ID3D11Device1> device1; const D3D_FEATURE_LEVEL level=device_->GetFeatureLevel(); D3D_FEATURE_LEVEL chosen{};
            if (FAILED(device_.As(&device1)) || FAILED(device1->CreateDeviceContextState(0,&level,1,D3D11_SDK_VERSION,__uuidof(ID3D11Device),&chosen,&workState_))) return Fail("full D3D11 state isolation unavailable");
        }
        if (!convert_) {
            ComPtr<ID3DBlob> code,errors;
            if (FAILED(D3DCompile(GuideShader,sizeof(GuideShader)-1,"F6 TAA guides",nullptr,nullptr,"main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors)) ||
                FAILED(device_->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&convert_))) return Fail("TAA guide shader creation failed");
            D3D11_BUFFER_DESC d{}; d.ByteWidth=32; d.Usage=D3D11_USAGE_DEFAULT; d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
            if (FAILED(device_->CreateBuffer(&d,nullptr,&settings_))) return Fail("TAA guide settings allocation failed");
        }
        bool changed=!allocated_ || madeTarget_.Width!=targetDesc_.Width || madeTarget_.Height!=targetDesc_.Height || madeTargetFormat_!=targetFormat_;
        for (unsigned i=0;i<3;++i) changed|=madeInputs_[i].Width!=input_[i].desc.Width || madeInputs_[i].Height!=input_[i].desc.Height || madeInputs_[i].Format!=input_[i].desc.Format || madeFormats_[i]!=input_[i].srv.Format;
        if (changed) {
            allocated_=false;
            for (auto& input:input_) { input.snapshot.Reset(); input.view.Reset(); }
            output_.Reset();depth_.Reset();motion_.Reset();depthUav_.Reset();motionUav_.Reset();
            if (!Allocate()) return false;
        }
        if (!done_) { D3D11_QUERY_DESC d{D3D11_QUERY_EVENT,0}; if (FAILED(device_->CreateQuery(&d,&done_))) return Fail("TAA completion query creation failed"); }
        frame_.color=input_[0].snapshot.Get();frame_.depth=depth_.Get();frame_.motion=motion_.Get();frame_.output=output_.Get();
        frame_.sourceColor=input_[0].source.Get();
        frame_.colorStorage=frame_.colorView=input_[0].srv.Format;
        frame_.sourceStorage=input_[0].desc.Format;frame_.sourceView=input_[0].srv.Format;
        recording_=true;
        for (auto& input:input_) context_->CopyResource(input.snapshot.Get(),input.source.Get());
        error_.clear(); return true;
    }
    using Evaluate = std::function<bool(const Frame&)>;
    const Frame& FrozenFrame() const {return frame_;}
    ID3D11Texture2D* OriginalTarget() const {return target_.Get();}
    ID3D11Texture2D* CurrentOutput() const {return output_.Get();}
    DXGI_FORMAT TargetFormat() const {return targetFormat_;}
    bool Resolve(unsigned width,unsigned height,const Evaluate& evaluate,bool copyBack) {
        if(width<frame_.renderWidth||height<frame_.renderHeight)return Fail("consumer extent is smaller than render input");
        if(copyBack&&(width!=targetDesc_.Width||height!=targetDesc_.Height))return Fail("copyback extent mismatch");
        if(!recording_||!output_)return false;
        D3D11_TEXTURE2D_DESC desc{};output_->GetDesc(&desc);
        if(width!=desc.Width||height!=desc.Height) {
            ComPtr<ID3D11Texture2D> resized;
            if(!Texture(resized,width,height,desc.Format,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS))return Fail("consumer SR output allocation failed");
            output_=std::move(resized);
        }
        frame_.output=output_.Get();frame_.outputWidth=width;frame_.outputHeight=height;
        bool currentOutput=false;
        {
            ContextStateGuard state(context1_.Get(),workState_.Get());
            struct Settings { UINT width,height,encoding,unused; float scaleX,scaleY,pad[2]; } settings{
                frame_.renderWidth,frame_.renderHeight,profile_.encoding==Encoding::SignedSquare?1u:0u,profile_.motionSampling==MotionSampling::NearestDepth5Max?1u:0u,
                PixelScale(profile_.motionUnits,frame_.renderWidth,profile_.motionScaleX),
                PixelScale(profile_.motionUnits,frame_.renderHeight,profile_.motionScaleY),{0,0}};
            context_->UpdateSubresource(settings_.Get(),0,nullptr,&settings,0,0);
            ID3D11ShaderResourceView* srvs[]{input_[1].view.Get(),input_[2].view.Get()};
            ID3D11UnorderedAccessView* uavs[]{depthUav_.Get(),motionUav_.Get()}; ID3D11Buffer* cb=settings_.Get();
            context_->CSSetShader(convert_.Get(),nullptr,0);context_->CSSetShaderResources(0,2,srvs);
            context_->CSSetUnorderedAccessViews(0,2,uavs,nullptr);context_->CSSetConstantBuffers(0,1,&cb);
            context_->Dispatch((frame_.renderWidth+7)/8,(frame_.renderHeight+7)/8,1);
            context_->ClearState();
            currentOutput=evaluate(frame_);
            context_->ClearState();
            if (currentOutput && copyBack) context_->CopyResource(target_.Get(),output_.Get());
        }
        if (!currentOutput) error_="SR did not prove this call's current output";
        return currentOutput;
    }
    bool Finish(const Evaluate& evaluate) {const bool current=Resolve(frame_.outputWidth,frame_.outputHeight,evaluate,true);Seal();return current;}
    void Abandon() { Seal(); }
};
} // namespace taa
