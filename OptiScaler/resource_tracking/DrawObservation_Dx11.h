// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>

// Optional draw-only consumers. This contract grants no SR/depth/FG authority.
// Registration tables have static product lifetime; there is no borrowed state
// pointer to an independently destructible consumer. UID results stay in UID.
namespace DrawObservationDx11 {
using Microsoft::WRL::ComPtr;
enum class Kind : unsigned { Draw=1, Indexed, Instanced, IndexedInstanced, Auto, IndexedIndirect, Indirect };
struct Draw {
    Kind kind=Kind::Draw;
    UINT count=0,start=0;
    INT base=0;
    UINT instances=1,instanceStart=0;
    bool countKnown=true;
    ID3D11Buffer* indirectArguments=nullptr; // borrowed ONLY for this callback
    UINT indirectOffset=0;
};
struct Owner {
    // Copy Owner to retain an asynchronous request. Never retain a callback's
    // reference or infer GPU completion from its frame/epoch.
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    uint64_t epoch=0,frame=0;
    DWORD thread=0;
    bool Matches(ID3D11DeviceContext* c) const noexcept {
        return epoch && device && context.Get()==c && thread==GetCurrentThreadId();
    }
};
enum class Completion : unsigned { Abandoned, OriginalReturned, ReplacementReturned, Consumed };
enum class Slot : unsigned { UiMask, Uid, Count };
struct ShaderIdentity { uint64_t hash=0,bytes=0; };
inline constexpr GUID ShaderIdentityKey={0xf566d101,0x4c11,0x46b3,{0xb9,0x18,0x56,0x60,0x11,0x00,0x00,0x01}};
inline void RememberShader(ID3D11DeviceChild* shader,uint64_t hash,size_t bytes) noexcept {
    if(shader && hash && bytes){const ShaderIdentity identity{hash,bytes};
        shader->SetPrivateData(ShaderIdentityKey,sizeof(identity),&identity);}
}
inline ShaderIdentity Identity(ID3D11DeviceChild* shader) noexcept {
    ShaderIdentity result{};UINT size=sizeof(result);
    if(!shader || FAILED(shader->GetPrivateData(ShaderIdentityKey,&size,&result)) || size!=sizeof(result))return {};
    return result;
}
// Shared lazy getters: candidate callbacks share ONE actual bound PS/VS query.
// Noncandidate draws never request them. Strong references survive both phases.
class CapturedState final {
    ID3D11DeviceContext* context_;
    bool psRead_=false,vsRead_=false;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11VertexShader> vs_;
public:
    explicit CapturedState(ID3D11DeviceContext* c) noexcept:context_(c){}
    ID3D11PixelShader* PixelShader() noexcept {
        if(!psRead_){psRead_=true;context_->PSGetShader(&ps_,nullptr,nullptr);}return ps_.Get();
    }
    ID3D11VertexShader* VertexShader() noexcept {
        if(!vsRead_){vsRead_=true;context_->VSGetShader(&vs_,nullptr,nullptr);}return vs_.Get();
    }
    ShaderIdentity PixelIdentity() noexcept {return Identity(PixelShader());}
    ShaderIdentity VertexIdentity() noexcept {return Identity(VertexShader());}
};
struct Consumer {
    void (*before)(ID3D11DeviceContext*,const Draw&,const Owner&,CapturedState&,uint64_t&) noexcept=nullptr;
    void (*after)(ID3D11DeviceContext*,const Draw&,const Owner&,uint64_t,Completion) noexcept=nullptr;
    void (*present)(ID3D11DeviceContext*,const Owner&) noexcept=nullptr;
    void (*revoke)() noexcept=nullptr;
    void (*nonDrawWrite)(ID3D11DeviceContext*) noexcept=nullptr;
};
inline std::array<std::atomic<const Consumer*>,2> consumers{};
inline std::atomic_uint active{0};
inline thread_local unsigned internalDepth=0;
class InternalScope final {
public: InternalScope() noexcept{++internalDepth;}~InternalScope(){--internalDepth;}
    InternalScope(const InternalScope&)=delete;InternalScope& operator=(const InternalScope&)=delete;
};
inline bool Internal() noexcept{return internalDepth!=0;}
// Configure/request boundaries only. The table must remain alive until DLL
// unload. Deactivation does not unpair already begun callbacks.
inline void Subscribe(Slot slot,const Consumer* table,bool enabled) noexcept {
    const auto index=static_cast<unsigned>(slot);if(index>=consumers.size())return;
    if(!enabled){active.fetch_and(~(1u<<index),std::memory_order_release);return;}
    if(table){consumers[index].store(table,std::memory_order_release);active.fetch_or(1u<<index,std::memory_order_release);}
}
inline bool NeedsDraw() noexcept{return active.load(std::memory_order_acquire)!=0;}
// One immutable strong owner per frame. Scope retains ONLY this C++ control
// block; no per-draw device/context AddRef/Release. UID copies Owner only for
// the actual bounded capture request it has accepted.
inline thread_local std::shared_ptr<const Owner> owner;
inline void Revoke() noexcept {
    for(auto& slot:consumers)if(const auto* c=slot.load(std::memory_order_acquire);c && c->revoke)c->revoke();
    owner.reset();
}
// Called once on the existing Present owner, before optional consumers run.
// The first frame is deliberately unavailable. This is a local draw epoch,
// independent of the native SR/NR capture owner's provenance.
inline void Present(ID3D11DeviceContext* context,uint64_t frame) noexcept {
    if(!NeedsDraw() || !context || Internal())return;
    if(owner && owner->Matches(context))for(auto& slot:consumers)
        if(const auto* c=slot.load(std::memory_order_acquire);c && c->present)c->present(context,*owner);
    // First establishment must not cancel a just-armed UID request. Only an
    // actually published, different owner constitutes revocation.
    if(owner && !owner->Matches(context))Revoke();
    try {
        auto nextOwner=std::make_shared<Owner>();nextOwner->context=context;
        context->GetDevice(&nextOwner->device);nextOwner->thread=GetCurrentThreadId();
        static std::atomic_uint64_t next{0};nextOwner->epoch=++next;nextOwner->frame=frame;
        owner=std::move(nextOwner);
    }catch(...){Revoke();}
}
class Scope final {
    ID3D11DeviceContext* context_=nullptr;
    Draw draw_{};
    std::shared_ptr<const Owner> owner_;
    std::array<const Consumer*,2> tables_{};
    std::array<uint64_t,2> cookies_{};
    Completion completion_=Completion::Abandoned;
public:
    // Lazy external predicate preserves the inactive R459 path: exactly one
    // active-mask load, with no extra legacy/internal lookup when it is zero.
    template<class External>
    Scope(ID3D11DeviceContext* context,const Draw& draw,External external) noexcept {
        const auto mask=active.load(std::memory_order_acquire);
        if(!mask || !external() || Internal() || !owner || !owner->Matches(context))return;
        context_=context;draw_=draw;owner_=owner;CapturedState state(context);
        for(unsigned i=0;i<tables_.size();++i)if(mask&(1u<<i)){
            tables_[i]=consumers[i].load(std::memory_order_acquire);
            if(tables_[i] && tables_[i]->before)tables_[i]->before(context,draw_,*owner_,state,cookies_[i]);
        }
    }
    Scope(ID3D11DeviceContext* context,const Draw& draw,bool external) noexcept
        :Scope(context,draw,[external]() noexcept{return external;}){}
    ~Scope(){Returned(Completion::Abandoned);}
    // Finish immediately after physical Original returns, BEFORE optional SR
    // replacement/internal work. Early-consumed branches also close exactly once.
    void Returned(Completion c=Completion::OriginalReturned) noexcept {
        if(!context_)return;completion_=c;
        for(unsigned i=0;i<tables_.size();++i)if(tables_[i] && tables_[i]->after)
            tables_[i]->after(context_,draw_,*owner_,cookies_[i],completion_);
        context_=nullptr;owner_.reset();
    }
    Scope(const Scope&)=delete;Scope& operator=(const Scope&)=delete;
};
inline void NonDrawWrite(ID3D11DeviceContext* context) noexcept {
    if(!NeedsDraw() || Internal() || !owner || !owner->Matches(context))return;
    const auto mask=active.load(std::memory_order_acquire);
    for(unsigned i=0;i<consumers.size();++i)if(mask&(1u<<i))
        if(const auto* c=consumers[i].load(std::memory_order_acquire);c && c->nonDrawWrite)c->nonDrawWrite(context);
}
} // namespace DrawObservationDx11
