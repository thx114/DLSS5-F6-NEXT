// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <runtime/ProducerProgress.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>

namespace GenshinLegacy::FsrPassObservation {
// Expensive missing-pass collection is an explicit diagnostic session, separate
// from the user's live HUD/Hook CPU controls. Read once at Present, not per draw.
inline bool ProcessRequested() noexcept {
    static const bool requested=[]() noexcept {
        const auto error=GetLastError();wchar_t value[2]{};
        const auto size=GetEnvironmentVariableW(L"OPTISCALER_FSR_PASS_OBSERVATION",value,2);
        SetLastError(error);return size==1 && value[0]==L'1';
    }();
    return requested;
}
// A structural observation of the reference bridge's fixed seven-input MRT
// layout. This is NOT a source schema, numerical profile or producer ticket.
struct Surface {
    uintptr_t resource=0;
    UINT width=0,height=0,format=0;
};
struct Facts {
    std::array<Surface,7> inputs{};
    std::array<Surface,2> outputs{};
    UINT cbBytes=0,viewportCount=0;
    D3D11_VIEWPORT viewport{};
    bool dual=false;
    // Independent failure bits preserve partial evidence without selecting a
    // different role or inventing formats when the fixed layout is absent.
    unsigned Failures() const noexcept {
        unsigned mask=0;
        bool present=true;for(const auto& s:inputs)present=present && s.resource && s.width && s.height;
        for(const auto& s:outputs)present=present && s.resource && s.width && s.height;
        if(!present)mask|=1;
        if(cbBytes<464)mask|=2;
        if(inputs[1].width!=16 || inputs[1].height!=16)mask|=4;
        const auto same=[](const Surface& a,const Surface& b){return a.width==b.width && a.height==b.height;};
        if(!same(inputs[0],inputs[2]) || !same(inputs[0],inputs[3]) || !same(inputs[0],inputs[4]))mask|=8;
        if(!same(outputs[0],outputs[1]) || !same(inputs[5],outputs[1]) || !same(inputs[6],outputs[1]))mask|=16;
        if(viewportCount!=1 || viewport.TopLeftX!=0 || viewport.TopLeftY!=0 ||
           viewport.Width!=outputs[1].width || viewport.Height!=outputs[1].height)mask|=32;
        if(inputs[0].width>outputs[1].width || inputs[0].height>outputs[1].height)mask|=64;
        return mask;
    }
};
template<class View> inline Surface Describe(View* view) noexcept {
    if(!view)return {};
    Microsoft::WRL::ComPtr<ID3D11Resource> resource;view->GetResource(&resource);
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    if(!resource || FAILED(resource.As(&texture)))return {};
    D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);
    // Raw allocation dimensions are reported, not inferred view semantics.
    return {reinterpret_cast<uintptr_t>(resource.Get()),d.Width,d.Height,static_cast<UINT>(d.Format)};
}
inline Facts Inspect(ID3D11DeviceContext* context) noexcept {
    Facts f;if(!context)return f;
    ID3D11RenderTargetView* rawOutputs[2]{};context->OMGetRenderTargets(2,rawOutputs,nullptr);
    std::array<Microsoft::WRL::ComPtr<ID3D11RenderTargetView>,2> outputs;
    for(unsigned i=0;i<2;++i)outputs[i].Attach(rawOutputs[i]);
    f.dual=bool(outputs[0]) && bool(outputs[1]);if(!f.dual)return f;
    // Single-RTV draws do not query any SRV, buffer or texture descriptor.
    ID3D11ShaderResourceView* rawInputs[7]{};context->PSGetShaderResources(0,7,rawInputs);
    for(unsigned i=0;i<7;++i){Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;view.Attach(rawInputs[i]);f.inputs[i]=Describe(view.Get());}
    for(unsigned i=0;i<2;++i)f.outputs[i]=Describe(outputs[i].Get());
    Microsoft::WRL::ComPtr<ID3D11Buffer> cb;context->PSGetConstantBuffers(0,1,&cb);
    if(cb){D3D11_BUFFER_DESC d{};cb->GetDesc(&d);f.cbBytes=d.ByteWidth;}
    UINT count=D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    std::array<D3D11_VIEWPORT,D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> vp{};
    context->RSGetViewports(&count,vp.data());f.viewportCount=count;if(count)f.viewport=vp[0];
    return f;
}
// A transient unknown/foreign Present leaves the record pending. Only its
// exact owner thread may consume it; real owner replacement discards the record.
class PendingDelivery final {
    bool emitted_=false;
public:
    bool TryEmit(const FramePipeline::ProducerProgress::Owner& expected,
                 const FramePipeline::ProducerProgress::Owner& current,uint64_t thread) noexcept {
        if(emitted_ || !expected.Valid() || expected!=current || thread!=expected.thread)return false;
        emitted_=true;return true;
    }
};
// Finite diagnostic coverage, independent of the older random cache samples.
// A recovered producer never replenishes this owner's scan allowance.
class ScanBudget final {
public:
    using Owner=FramePipeline::ProducerProgress::Owner;
    static constexpr unsigned MaxDraws=20000;
    inline static constexpr std::array<uint64_t,4> Offsets{4,64,256,512};
private:
    Owner owner_{};
    uint64_t opened_=0;
    unsigned windows_=0,draws_=0;
    bool active_=false;
public:
    bool SameOwner(const Owner& o)const noexcept{return owner_==o;}
    bool Active(const Owner& o)const noexcept{return active_ && owner_==o && draws_<MaxDraws;}
    bool Ends(const Owner& o,uint64_t render)const noexcept{return active_ && (owner_!=o || render!=opened_);}
    unsigned Draws()const noexcept{return draws_;}
    uint64_t Opened()const noexcept{return opened_;}
    void Advance(const Owner& o,uint64_t render,uint64_t produced,bool stalled) noexcept {
        // Unknown intervals close observation but cannot replenish the same
        // owner's lifetime allowance when its next valid snapshot arrives.
        if(!o.Valid() || !render || render==UINT64_MAX){active_=false;return;}
        if(o!=owner_){*this={};owner_=o;}
        if(active_ && render!=opened_)active_=false;
        if(!active_ && o.Valid() && render && render!=UINT64_MAX && produced && produced<=render &&
           stalled && windows_<Offsets.size() && render-produced>=Offsets[windows_]) {
            active_=true;opened_=render;draws_=0;++windows_;
        }
    }
    bool Spend(const Owner& o)noexcept{if(!Active(o))return false;++draws_;return true;}
};
}
