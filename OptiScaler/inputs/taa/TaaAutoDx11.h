// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "TaaShaderAnalysis.h"
#include "TaaConstantBuffer.h"
#include "TaaProjectionJitterDx11.h"
#include "TaaConsumerDx11.h"
#include <atomic>
#include <mutex>
#include <format>

namespace taa::automatic {
using Microsoft::WRL::ComPtr;
inline constexpr GUID ProgramKey={0xf6aa0002,0x928b,0x4cb7,{0xa5,0x91,0xda,0x11,0x71,0x21,0x83,0x05}};
inline constexpr GUID DepthKey={0xf6aa0003,0x928b,0x4cb7,{0xa5,0x91,0xda,0x11,0x71,0x21,0x83,0x05}};
class Program final : public IUnknown {
    std::atomic<ULONG> refs_{1};
    std::once_flag once_;
    std::vector<unsigned char> bytes_;
    std::string assembly_;
    analysis::Result result_;
    std::mutex evidenceMutex_;
    std::optional<std::pair<uint64_t,unsigned>> attempted_;
    bool exported_=false;
public:
    const jitter::Program projection;
    const consumer::Program postPixel,postVertex;
    Program(const void* data,size_t size):bytes_(static_cast<const unsigned char*>(data),static_cast<const unsigned char*>(data)+size),projection(jitter::Identify(data,size)),postPixel(consumer::Describe(data,size,consumer::ProgramStage::Pixel)),postVertex(consumer::Describe(data,size,consumer::ProgramStage::Vertex)){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override {
        if(!out)return E_POINTER;*out=nullptr;if(id!=__uuidof(IUnknown))return E_NOINTERFACE;
        *out=static_cast<IUnknown*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return ++refs_;}
    ULONG STDMETHODCALLTYPE Release() override {const auto n=--refs_;if(!n)delete this;return n;}
    const analysis::Result& Analyze(Stage stage,uint64_t hash) {
        std::call_once(once_,[&]{
            result_=analysis::Analyze(bytes_.data(),bytes_.size(),stage,hash,&assembly_);
        });
        return result_;
    }
    template<class Export> const analysis::Result& Analyze(Stage stage,uint64_t hash,Export&& capture,
        std::pair<uint64_t,unsigned> checkpoint={}) {
        Analyze(stage,hash);
        std::lock_guard lock(evidenceMutex_);
        if(exported_||attempted_==checkpoint)return result_;
        attempted_=checkpoint;
        // Analysis and diagnostic submission have separate lifetimes. A full
        // queue may retry at the next bounded capture checkpoint; successful
        // submission releases the original bytes exactly once.
        auto bytes=bytes_;auto assembly=assembly_;
        if(capture(std::move(bytes),std::move(assembly),result_)) {
            exported_=true;std::vector<unsigned char>().swap(bytes_);std::string().swap(assembly_);
        }
        return result_;
    }
};
inline void Remember(ID3D11DeviceChild* shader,const void* data,size_t size) noexcept {
    try {
        if(!shader||!data||size<32||size>4*1024*1024||std::memcmp(data,"DXBC",4))return;
        ComPtr<Program> program;program.Attach(new Program(data,size));shader->SetPrivateDataInterface(ProgramKey,program.Get());
    } catch(...) {}
}
inline ComPtr<Program> Find(ID3D11DeviceChild* shader) {
    ComPtr<Program> result;IUnknown* object=nullptr;UINT size=sizeof(object);
    if(shader&&SUCCEEDED(shader->GetPrivateData(ProgramKey,&size,&object))&&object)result.Attach(static_cast<Program*>(object));
    return result;
}
// This records an ordered clear on the *same* native depth resource. It grants
// no completion or lifetime authority; the normal transaction still owns it.
inline void DepthCleared(ID3D11DeviceContext* context,ID3D11DepthStencilView* view,UINT flags,float value) noexcept {
    if(!context||context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE||!view||!(flags&D3D11_CLEAR_DEPTH))return;
    ComPtr<ID3D11Resource> resource;view->GetResource(&resource);
    const int convention=value==0?1:value==1?0:-1;
    if(resource)resource->SetPrivateData(DepthKey,sizeof(convention),&convention);
}
inline bool ReadConstant(ID3D11DeviceContext* context,Stage stage,analysis::Constant field,float& value) {
    ComPtr<ID3D11DeviceContext1> c;ComPtr<ID3D11Buffer> buffer;UINT first=0,count=0;
    if(FAILED(context->QueryInterface(IID_PPV_ARGS(&c))))return false;
    if(stage==Stage::Pixel)c->PSGetConstantBuffers1(field.slot,1,&buffer,&first,&count);
    else c->CSGetConstantBuffers1(field.slot,1,&buffer,&first,&count);
    return buffer&&field.offset/16<count&&constants::Read(buffer.Get(),first*16+field.offset,value);
}
struct BoundTexture {
    ComPtr<ID3D11Texture2D> texture;
    D3D11_TEXTURE2D_DESC desc{};
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
};
inline bool Color(DXGI_FORMAT f) {
    return f==DXGI_FORMAT_R16G16B16A16_FLOAT||f==DXGI_FORMAT_R32G32B32A32_FLOAT||f==DXGI_FORMAT_R11G11B10_FLOAT||
        f==DXGI_FORMAT_R10G10B10A2_UNORM||f==DXGI_FORMAT_R8G8B8A8_UNORM||f==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB||
        f==DXGI_FORMAT_B8G8R8A8_UNORM||f==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}
inline bool DeviceDepth(const BoundTexture& t) {
    return t.texture&&(t.desc.BindFlags&D3D11_BIND_DEPTH_STENCIL)&&
        (t.view.Format==DXGI_FORMAT_R32_FLOAT||t.view.Format==DXGI_FORMAT_R16_UNORM||
         t.view.Format==DXGI_FORMAT_R24_UNORM_X8_TYPELESS||t.view.Format==DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS);
}
struct Resolution {
    std::optional<Profile> profile;
    unsigned width=0,height=0;
    std::string reason,bindings;
};
inline Resolution Resolve(ID3D11DeviceContext* context,Stage stage,const analysis::Result& shader,jitter::Input jitter={}) {
    Resolution out;out.reason=shader.reason;
    std::array<ID3D11ShaderResourceView*,128> views{};
    // One getter for all slots; no 128 individual COM queries in the draw path.
    if(stage==Stage::Pixel)context->PSGetShaderResources(0,UINT(views.size()),views.data());
    else context->CSGetShaderResources(0,UINT(views.size()),views.data());
    std::array<ComPtr<ID3D11ShaderResourceView>,128> held;
    std::array<BoundTexture,128> textures;
    for(unsigned i=0;i<views.size();++i) {
        held[i].Attach(views[i]);if(!held[i])continue;
        auto& t=textures[i];ComPtr<ID3D11Resource> resource;held[i]->GetResource(&resource);
        if(FAILED(resource.As(&t.texture)))continue;
        t.texture->GetDesc(&t.desc);held[i]->GetDesc(&t.view);
        if(i<16||std::find(shader.textures.begin(),shader.textures.end(),i)!=shader.textures.end())
            out.bindings+=std::format("t{}={}x{}:{} ",i,t.desc.Width,t.desc.Height,unsigned(t.view.Format));
    }
    for(const auto& recipe:shader.recipes) {
        auto p=recipe.profile;const auto& color=textures[p.colorSlot];const auto& history=textures[recipe.historySlot];const auto& motion=textures[p.motionSlot];
        if(!color.texture||!history.texture||!motion.texture||!Color(color.view.Format)||!Color(history.view.Format)) {out.reason="Temporal textures are not bound";continue;}
        out.width=color.desc.Width;out.height=color.desc.Height;
        if(motion.desc.Width!=out.width||motion.desc.Height!=out.height) {out.reason="Motion does not match the current color extent";continue;}
        if(recipe.validatedProjection&&color.texture==history.texture){out.reason="Temporal history aliases current color";continue;}
        std::optional<unsigned> depthSlot;bool ambiguous=false;
        for(const auto slot:shader.textures) {
            if(slot==p.colorSlot||slot==recipe.historySlot||slot==p.motionSlot)continue;
            const auto& depth=textures[slot];
            if(!DeviceDepth(depth)||depth.desc.Width!=out.width||depth.desc.Height!=out.height)continue;
            if(depthSlot&&*depthSlot!=slot)ambiguous=true;else depthSlot=slot;
        }
        if(!depthSlot||ambiguous) {out.reason="A unique matching native device-depth input is required";continue;}
        p.depthSlot=*depthSlot;
        std::optional<bool> reversed;
        int convention=-1;UINT length=sizeof(convention);
        if(SUCCEEDED(textures[*depthSlot].texture->GetPrivateData(DepthKey,&length,&convention))&&length==sizeof(convention)&&convention>=0)
            reversed=convention!=0;
        if(!reversed&&recipe.zParameters) {
            float x=0,y=0;auto second=*recipe.zParameters;second.offset+=4;
            if(ReadConstant(context,p.stage,*recipe.zParameters,x)&&ReadConstant(context,p.stage,second,y)&&std::isfinite(x)&&std::isfinite(y)&&std::abs(x+y-1)<.01f)
                reversed=x>0;
        }
        if(recipe.validatedProjection) {
            const auto* sample=jitter.sample;
            if(!sample||!sample->validatedDepthConvention||!sample->Matches(context,jitter.revision,motion.texture.Get(),textures[*depthSlot].texture.Get(),out.width,out.height,motion.view.Format)) {
                out.reason="Waiting for current packed-motion projection contract";continue;
            }
            float x=0,y=0,u=0,v=0,reset=0;
            if(!ReadConstant(context,p.stage,{0,2240},x)||!ReadConstant(context,p.stage,{0,2244},y)||
                !ReadConstant(context,p.stage,{0,2248},u)||!ReadConstant(context,p.stage,{0,2252},v)||!ReadConstant(context,p.stage,{0,2672},reset)) {
                out.reason="Waiting for packed resolve constants";continue;
            }
            if(!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(u)||!std::isfinite(v)||!std::isfinite(reset)||reset>.5f||
                std::abs(x+sample->offset.x)>.005f||std::abs(y+sample->offset.y)>.005f||
                std::abs(u*out.width-x)>.005f||std::abs(v*out.height-y)>.005f) {
                out.reason="Packed resolve projection/reset does not match current producer";continue;
            }
            reversed=sample->validatedDepthConvention;
        }
        if(!reversed){out.reason="Waiting for native depth convention evidence";continue;}
        p.depthInverted=*reversed;
        if(!recipe.jitterInResolve) {
            if(!jitter.sample||!jitter.sample->Matches(context,jitter.revision,motion.texture.Get(),textures[*depthSlot].texture.Get(),out.width,out.height,motion.view.Format)) {
                out.reason="Temporal reprojection identified; waiting for same-frame raster projection jitter";continue;
            }
            p.jitterSource=JitterSource::MotionProjection;
        } else {
            float x=0,y=0;
            const bool haveX=ReadConstant(context,p.stage,{p.jitterSlot,p.jitterOffsetX},x);
            const bool haveY=ReadConstant(context,p.stage,{p.jitterSlot,p.jitterOffsetY},y);
            if(!haveX||!haveY){out.reason="Waiting for a current write of the analyzed jitter buffer";continue;}
            x*=PixelScale(p.jitterUnits,out.width,p.jitterScaleX);y*=PixelScale(p.jitterUnits,out.height,p.jitterScaleY);
            if(!std::isfinite(x)||!std::isfinite(y)||std::abs(x)>2||std::abs(y)>2){out.reason="Analyzed jitter is outside the supported pixel range";continue;}
        }
        if(out.profile&&*out.profile!=p){out.profile.reset();out.reason="Multiple temporal contracts are still ambiguous";return out;}
        out.profile=p;out.reason="Temporal inputs analyzed; visual validation is still required";
    }
    return out;
}
} // namespace taa::automatic
