// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <resource_tracking/DrawObservation_Dx11.h>
#include <inputs/GenshinFsrContract.h>
#include <d3d11_1.h>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string_view>

// Draw-only, explicitly requested, process-memory-only UID acquisition.
// There is currently NO proven game glyph decoder. RawEvidence is not a UID,
// an account identity, an authorization result, or an NR/FG/filter UI mask.
namespace DrawUid {
using DrawObservationDx11::ComPtr;
using DrawObservationDx11::Draw;
using DrawObservationDx11::Owner;
using DrawObservationDx11::CapturedState;
inline constexpr UINT MaxBytes=4096;
inline constexpr unsigned MaxPresents=120;
inline constexpr ULONGLONG MaxRequestMilliseconds=10000;

struct Selection {
    // Actual creation-time shader identities supplied by the shared observer.
    // No guessed shader, texture colour, font atlas or scene-PS fallback.
    uint64_t pixelShader=0,vertexShader=0;
    UINT indexCount=0,startIndex=0;
    INT baseVertex=0;
    UINT vertexSlot=0,vertexByteOffset=0,vertexBytes=0;
    // Optional exact CB range; zero bytes means do not touch any CB.
    bool pixelConstants=false;
    UINT constantSlot=0,constantByteOffset=0,constantBytes=0;
};
enum class Phase : unsigned char {
    Disabled, Idle, Requested, Armed, Pending, EvidenceAvailable, Unknown, Quarantined
};
enum class Reason : unsigned char {
    None, WrongGame, ExplicitTargetRequired, InvalidRange, AwaitingPresent,
    AwaitingExactDraw, NoMatch, SourceIdentityUnavailable, MissingBindings,
    UnsupportedBinding, Ambiguous, DrawNotOriginal, OwnerChanged, Cancelled,
    CpuContention, Deadline, GpuPending, ReadbackFailed, GlyphContractMissing
};
struct Snapshot {
    Phase phase=Phase::Disabled;
    Reason reason=Reason::None;
    bool enabled=false,hasUid=false,rawAvailable=false,pendingGpu=false;
    // Always empty until a real, draw-bound, proven game glyph decoder exists.
    char uid[17]{};
    uint64_t request=0,ownerEpoch=0,frame=0,pixelShader=0,vertexShader=0;
    unsigned matchingDraws=0,presents=0;
    UINT vertexBytes=0,indexBytes=0,constantBytes=0,stride=0;
    HRESULT result=S_OK;
};
struct RawEvidence {
    Snapshot facts{};
    Selection selection{};
    UINT vertexBindingOffset=0,indexBindingOffset=0;
    DXGI_FORMAT indexFormat=DXGI_FORMAT_UNKNOWN;
    D3D11_PRIMITIVE_TOPOLOGY topology=D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    // Exact requested source ranges only. No inferred glyph or vertex extent.
    std::array<unsigned char,MaxBytes> vertices{},indices{},constants{};
};

inline std::atomic_bool drawDemand{false};
inline bool NeedsDraw() noexcept {return drawDemand.load(std::memory_order_acquire);}

namespace detail {
inline bool IsGenshin() noexcept {
    static const bool yes=[] {
        wchar_t path[32768]{};const auto length=GetModuleFileNameW(nullptr,path,32768);
        return length && length<32768 && GenshinFsr::IsGenshinExecutable(std::wstring_view(path,length));
    }();
    return yes;
}
inline void Erase(RawEvidence& value) noexcept {SecureZeroMemory(&value,sizeof(value));}
inline bool Range(UINT capacity,uint64_t offset,uint64_t bytes) noexcept {
    return bytes && bytes<=MaxBytes && offset<=capacity && bytes<=capacity-offset;
}
inline bool Valid(const Selection& s) noexcept {
    return s.pixelShader && s.vertexShader && s.indexCount && s.indexCount<=MaxBytes/2 &&
        s.vertexSlot<D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT && s.vertexBytes && s.vertexBytes<=MaxBytes &&
        s.constantBytes<=MaxBytes && (!s.constantBytes || s.constantSlot<D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT);
}

// One slot, allocated on the first UI access and intentionally process-lived.
// An uncompleted submission is never released by static/DLL teardown. There is
// no unbounded list: a quarantined slot prevents subsequent requests.
class Runtime final {
    std::mutex mutex_;
    bool enabled_=false,requested_=false,inFlight_=false,sealed_=false,discard_=false,quarantined_=false;
    bool rawAvailable_=false;
    unsigned presents_=0,hits_=0;
    uint64_t nextRequest_=0,cookie_=0;
    ULONGLONG requestedAt_=0;
    Selection selection_{};
    Snapshot snapshot_{};
    RawEvidence raw_{};
    Owner owner_{};
    ComPtr<ID3D11Buffer> vertex_,index_,constant_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11InputLayout> layout_;
    std::array<ComPtr<ID3D11Buffer>,3> staging_{};
    ComPtr<ID3D11Query> completion_;
    // UI changes cannot race hot-path scalar reads. Everything else is read
    // only after the try-lock, and a missed callback poisons this request.
    std::atomic_uint count_{0},start_{0};
    std::atomic_int base_{0};
    std::atomic_bool cancelled_{false},poisoned_{false};

    static bool SameOwner(const Owner& a,const Owner& b) noexcept {
        // The shared observer increments epoch at EACH Present; it is a draw
        // interval, not a stable device generation or a GPU completion value.
        return a.device && a.context && a.device.Get()==b.device.Get() &&
            a.context.Get()==b.context.Get() && a.thread==b.thread;
    }
    void Publish() noexcept {
        snapshot_.enabled=enabled_;snapshot_.pendingGpu=inFlight_;
        snapshot_.rawAvailable=rawAvailable_;snapshot_.matchingDraws=hits_;snapshot_.presents=presents_;
        snapshot_.hasUid=false;snapshot_.uid[0]='\0';
    }
    void DropRaw() noexcept {rawAvailable_=false;Erase(raw_);}
    void ReleaseCompleted() noexcept {
        // ONLY before submission, after actual query completion, or after an
        // explicit failed GetDeviceRemovedReason; never merely on timeout.
        vertex_.Reset();index_.Reset();constant_.Reset();vs_.Reset();ps_.Reset();layout_.Reset();
        for(auto& s:staging_)s.Reset();completion_.Reset();owner_={};
        inFlight_=false;sealed_=false;cookie_=0;
    }
    void Finish(Reason reason) noexcept {
        drawDemand.store(false,std::memory_order_release);requested_=false;
        snapshot_.phase=enabled_?Phase::Unknown:Phase::Disabled;snapshot_.reason=reason;
        if(inFlight_)discard_=true;
        else {ReleaseCompleted();DrawObservationDx11::Subscribe(DrawObservationDx11::Slot::Uid,nullptr,false);}
        Publish();
    }
    void Quarantine(Reason reason,HRESULT result) noexcept {
        drawDemand.store(false,std::memory_order_release);requested_=false;discard_=true;
        quarantined_=true;DropRaw();snapshot_.phase=Phase::Quarantined;
        snapshot_.reason=reason;snapshot_.result=result;Publish();
        DrawObservationDx11::Subscribe(DrawObservationDx11::Slot::Uid,nullptr,false);
        // All owners, source resources, staging and the query remain retained.
    }
    bool Prepare(const Owner& owner) noexcept {
        if(!owner.Matches(owner.context.Get()) || !owner.device ||
            owner.context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return false;
        ComPtr<ID3D11Device> actual;owner.context->GetDevice(&actual);
        if(actual.Get()!=owner.device.Get())return false;
        owner_=owner;
        DrawObservationDx11::InternalScope internal;
        D3D11_BUFFER_DESC desc{MaxBytes,D3D11_USAGE_STAGING,0,D3D11_CPU_ACCESS_READ,0,0};
        const unsigned count=selection_.constantBytes?3:2;
        for(unsigned i=0;i<count;++i){
            if(i==2)desc.ByteWidth=selection_.constantBytes;
            snapshot_.result=owner_.device->CreateBuffer(&desc,nullptr,&staging_[i]);
            if(FAILED(snapshot_.result))return false;
        }
        const D3D11_QUERY_DESC query{D3D11_QUERY_EVENT,0};
        snapshot_.result=owner_.device->CreateQuery(&query,&completion_);
        return SUCCEEDED(snapshot_.result);
    }
    bool Capture(ID3D11DeviceContext* context,const Draw& draw,const Owner& owner,CapturedState& state) noexcept {
        UINT stride=0,vertexOffset=0,indexOffset=0;
        DXGI_FORMAT indexFormat=DXGI_FORMAT_UNKNOWN;
        context->IAGetVertexBuffers(selection_.vertexSlot,1,&vertex_,&stride,&vertexOffset);
        context->IAGetIndexBuffer(&index_,&indexFormat,&indexOffset);
        context->IAGetInputLayout(&layout_);
        context->IAGetPrimitiveTopology(&raw_.topology);
        vs_=state.VertexShader();ps_=state.PixelShader();
        if(!vertex_ || !index_ || !layout_ || !stride || !vs_ || !ps_){Finish(Reason::MissingBindings);return false;}
        UINT indexSize=0;
        if(indexFormat==DXGI_FORMAT_R16_UINT)indexSize=2;
        else if(indexFormat==DXGI_FORMAT_R32_UINT)indexSize=4;
        else {Finish(Reason::UnsupportedBinding);return false;}
        D3D11_BUFFER_DESC vb{},ib{},cb{};vertex_->GetDesc(&vb);index_->GetDesc(&ib);
        const uint64_t vbegin=uint64_t(vertexOffset)+selection_.vertexByteOffset;
        const uint64_t ibegin=uint64_t(indexOffset)+uint64_t(draw.start)*indexSize;
        const uint64_t ibytes=uint64_t(draw.count)*indexSize;
        if(!Range(vb.ByteWidth,vbegin,selection_.vertexBytes) || !Range(ib.ByteWidth,ibegin,ibytes) ||
            !(vb.BindFlags&D3D11_BIND_VERTEX_BUFFER) || !(ib.BindFlags&D3D11_BIND_INDEX_BUFFER)){
            Finish(Reason::InvalidRange);return false;
        }
        if(selection_.constantBytes){
            if(selection_.pixelConstants)context->PSGetConstantBuffers(selection_.constantSlot,1,&constant_);
            else context->VSGetConstantBuffers(selection_.constantSlot,1,&constant_);
            if(!constant_){Finish(Reason::MissingBindings);return false;}
            constant_->GetDesc(&cb);
            // Base D3D11 constant buffers permit only a full-resource copy.
            // Prepare already allocated an exactly sized staging destination.
            if(!(cb.BindFlags&D3D11_BIND_CONSTANT_BUFFER) || selection_.constantByteOffset ||
                selection_.constantBytes!=cb.ByteWidth){Finish(Reason::UnsupportedBinding);return false;}
            // Ranged D3D11.1 CB binding offsets are not guessed by this slice.
            ComPtr<ID3D11DeviceContext1> context1;
            if(SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&context1)))){
                UINT first=0,number=0;ComPtr<ID3D11Buffer> ranged;
                if(selection_.pixelConstants)context1->PSGetConstantBuffers1(selection_.constantSlot,1,&ranged,&first,&number);
                else context1->VSGetConstantBuffers1(selection_.constantSlot,1,&ranged,&first,&number);
                if(ranged.Get()!=constant_.Get() || first || uint64_t(number)*16<cb.ByteWidth){Finish(Reason::UnsupportedBinding);return false;}
            }
            if(!Range(cb.ByteWidth,selection_.constantByteOffset,selection_.constantBytes)){
                Finish(Reason::InvalidRange);return false;
            }
        }
        raw_.selection=selection_;raw_.vertexBindingOffset=vertexOffset;
        raw_.indexBindingOffset=indexOffset;raw_.indexFormat=indexFormat;
        snapshot_.vertexBytes=selection_.vertexBytes;snapshot_.indexBytes=static_cast<UINT>(ibytes);
        snapshot_.constantBytes=selection_.constantBytes;snapshot_.stride=stride;
        snapshot_.ownerEpoch=owner.epoch;snapshot_.frame=owner.frame;
        snapshot_.pixelShader=selection_.pixelShader;snapshot_.vertexShader=selection_.vertexShader;
        // Strong ownership is committed BEFORE the first queued GPU command.
        // Copy freezes the requested bytes at this actual draw, not at Present.
        owner_=owner;inFlight_=true;sealed_=false;cookie_=snapshot_.request;
        snapshot_.phase=Phase::Pending;snapshot_.reason=Reason::GpuPending;
        DrawObservationDx11::InternalScope internal;
        const D3D11_BOX vertexBox{static_cast<UINT>(vbegin),0,0,static_cast<UINT>(vbegin+selection_.vertexBytes),1,1};
        const D3D11_BOX indexBox{static_cast<UINT>(ibegin),0,0,static_cast<UINT>(ibegin+ibytes),1,1};
        context->CopySubresourceRegion(staging_[0].Get(),0,0,0,0,vertex_.Get(),0,&vertexBox);
        context->CopySubresourceRegion(staging_[1].Get(),0,0,0,0,index_.Get(),0,&indexBox);
        if(constant_){
            context->CopyResource(staging_[2].Get(),constant_.Get());
        }
        // End is void, so its CPU return proves no completion or submission.
        // A failed/unobservable completion keeps these real owners quarantined.
        context->End(completion_.Get());Publish();return true;
    }
    void Poll(ID3D11DeviceContext* context) noexcept {
        DrawObservationDx11::InternalScope internal;
        BOOL completed=FALSE;
        const auto hr=context->GetData(completion_.Get(),&completed,sizeof(completed),D3D11_ASYNC_GETDATA_DONOTFLUSH);
        snapshot_.result=hr;
        if(hr==S_FALSE || (hr==S_OK && !completed))return;
        if(hr!=S_OK){
            const auto removed=owner_.device->GetDeviceRemovedReason();
            if(FAILED(removed)){inFlight_=false;ReleaseCompleted();Finish(Reason::ReadbackFailed);}
            else Quarantine(Reason::ReadbackFailed,hr);
            return;
        }
        inFlight_=false; // Actual query completion, not Present or CPU return.
        if(discard_){ReleaseCompleted();Publish();return;}
        const unsigned count=selection_.constantBytes?3:2;
        unsigned char* output[]{raw_.vertices.data(),raw_.indices.data(),raw_.constants.data()};
        const UINT bytes[]{snapshot_.vertexBytes,snapshot_.indexBytes,snapshot_.constantBytes};
        for(unsigned i=0;i<count;++i){
            D3D11_MAPPED_SUBRESOURCE mapped{};
            snapshot_.result=context->Map(staging_[i].Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);
            if(snapshot_.result!=S_OK || !mapped.pData){
                if(snapshot_.result==S_OK)context->Unmap(staging_[i].Get(),0);
                DropRaw();ReleaseCompleted();Finish(Reason::ReadbackFailed);return;
            }
            std::memcpy(output[i],mapped.pData,bytes[i]);context->Unmap(staging_[i].Get(),0);
        }
        rawAvailable_=true;snapshot_.phase=Phase::EvidenceAvailable;snapshot_.reason=Reason::GlyphContractMissing;
        Publish();raw_.facts=snapshot_;ReleaseCompleted();
        // Intentionally NO ASCII, texture, UV/atlas, position or colour heuristic.
        // A proven shader/layout/glyph contract is required before hasUid=true.
    }
public:
    void Enable(bool enabled) noexcept {
        std::lock_guard lock(mutex_);enabled_=enabled;
        if(!enabled){cancelled_.store(true,std::memory_order_release);drawDemand.store(false,std::memory_order_release);
            DropRaw();if(!quarantined_){snapshot_.phase=Phase::Disabled;snapshot_.reason=Reason::Cancelled;}}
        else if(!requested_ && !inFlight_ && !quarantined_){snapshot_.phase=Phase::Idle;snapshot_.reason=Reason::None;}
        Publish();
    }
    bool Request(const Selection& selection) noexcept {
        std::lock_guard lock(mutex_);
        if(!enabled_ || requested_ || inFlight_ || quarantined_)return false;
        DropRaw();snapshot_={};snapshot_.enabled=enabled_;
        if(!IsGenshin()){snapshot_.phase=Phase::Unknown;snapshot_.reason=Reason::WrongGame;return false;}
        if(!selection.pixelShader || !selection.vertexShader){snapshot_.phase=Phase::Unknown;snapshot_.reason=Reason::ExplicitTargetRequired;return false;}
        if(!Valid(selection)){snapshot_.phase=Phase::Unknown;snapshot_.reason=Reason::InvalidRange;return false;}
        ReleaseCompleted();selection_=selection;requested_=true;discard_=false;sealed_=false;
        cancelled_.store(false,std::memory_order_release);poisoned_.store(false,std::memory_order_release);
        presents_=hits_=0;requestedAt_=GetTickCount64();snapshot_.request=++nextRequest_;
        count_.store(selection.indexCount,std::memory_order_relaxed);start_.store(selection.startIndex,std::memory_order_relaxed);
        base_.store(selection.baseVertex,std::memory_order_relaxed);
        snapshot_.phase=Phase::Requested;snapshot_.reason=Reason::AwaitingPresent;Publish();return true;
    }
    void Before(ID3D11DeviceContext* context,const Draw& draw,const Owner& owner,CapturedState& state,uint64_t& cookie) noexcept {
        cookie=0;
        if(!NeedsDraw() || draw.kind!=DrawObservationDx11::Kind::Indexed || !draw.countKnown ||
            draw.instances!=1 || draw.instanceStart || draw.count!=count_.load(std::memory_order_relaxed) ||
            draw.start!=start_.load(std::memory_order_relaxed) || draw.base!=base_.load(std::memory_order_relaxed))return;
        std::unique_lock lock(mutex_,std::try_to_lock);
        if(!lock.owns_lock()){poisoned_.store(true,std::memory_order_release);return;}
        if(!requested_ || cancelled_.load(std::memory_order_acquire) || !enabled_ ||
            !owner.Matches(context) || !SameOwner(owner_,owner))return;
        const auto pixel=state.PixelIdentity(),vertex=state.VertexIdentity();
        if(!pixel.hash || !pixel.bytes || !vertex.hash || !vertex.bytes){
            snapshot_.reason=Reason::SourceIdentityUnavailable;return;
        }
        if(pixel.hash!=selection_.pixelShader || vertex.hash!=selection_.vertexShader)return;
        ++hits_;
        if(hits_!=1){DropRaw();Finish(Reason::Ambiguous);return;}
        if(Capture(context,draw,owner,state))cookie=cookie_;
    }
    void After(ID3D11DeviceContext* context,const Owner& owner,uint64_t cookie,DrawObservationDx11::Completion completion) noexcept {
        if(!cookie)return;
        std::unique_lock lock(mutex_,std::try_to_lock);
        if(!lock.owns_lock()){poisoned_.store(true,std::memory_order_release);return;}
        if(cookie!=cookie_ || !inFlight_)return;
        if(!owner.Matches(context) || !SameOwner(owner_,owner) || owner.epoch!=snapshot_.ownerEpoch ||
            completion!=DrawObservationDx11::Completion::OriginalReturned){Finish(Reason::DrawNotOriginal);return;}
        // Pairing proves CPU original-draw return only. The query still owns GPU work.
        sealed_=true;
    }
    void Present(ID3D11DeviceContext* context,const Owner& owner) noexcept {
        std::lock_guard lock(mutex_);
        if(quarantined_)return;
        if(!requested_ && !inFlight_)return;
        if(!owner.Matches(context))return;
        ++presents_;
        if(!owner_.context){
            if(cancelled_.load(std::memory_order_acquire) || !enabled_){Finish(Reason::Cancelled);return;}
            if(GetTickCount64()-requestedAt_>=MaxRequestMilliseconds){Finish(Reason::Deadline);return;}
            if(!Prepare(owner)){Finish(Reason::MissingBindings);return;}
            snapshot_.phase=Phase::Armed;snapshot_.reason=Reason::AwaitingExactDraw;
            drawDemand.store(true,std::memory_order_release);Publish();return;
        }
        if(!SameOwner(owner_,owner)){
            if(inFlight_)Quarantine(Reason::OwnerChanged,E_PENDING);else Finish(Reason::OwnerChanged);
            return;
        }
        if(cancelled_.load(std::memory_order_acquire) || !enabled_)Finish(Reason::Cancelled);
        else if(poisoned_.exchange(false,std::memory_order_acq_rel))Finish(Reason::CpuContention);
        if(inFlight_){
            // First authentic Present seals the complete capture interval and
            // rejects ambiguity BEFORE a later Present can expose its bytes.
            if(NeedsDraw()){
                drawDemand.store(false,std::memory_order_release);requested_=false;
                if(hits_!=1 || !sealed_)Finish(Reason::DrawNotOriginal);
                Publish();return;
            }
            Poll(context);
            if(inFlight_ && (presents_>=MaxPresents || GetTickCount64()-requestedAt_>=MaxRequestMilliseconds))
                Quarantine(Reason::Deadline,E_PENDING);
            Publish();return;
        }
        if(requested_ && (presents_>=MaxPresents || GetTickCount64()-requestedAt_>=MaxRequestMilliseconds)){
            const auto reason=snapshot_.reason==Reason::SourceIdentityUnavailable?Reason::SourceIdentityUnavailable:Reason::NoMatch;
            Finish(reason);
        }
        Publish();
    }
    void Revoke() noexcept {
        cancelled_.store(true,std::memory_order_release);drawDemand.store(false,std::memory_order_release);
        // The shared observer distinguishes first establishment from a real
        // owner boundary; every Revoke here really cancels this request.
        std::unique_lock lock(mutex_,std::try_to_lock);
        if(!lock.owns_lock()){poisoned_.store(true,std::memory_order_release);return;}
        if(quarantined_){DropRaw();Publish();return;}
        if(!requested_ && !inFlight_){
            if(rawAvailable_){DropRaw();snapshot_.phase=enabled_?Phase::Unknown:Phase::Disabled;
                snapshot_.reason=Reason::OwnerChanged;Publish();}
            return;
        }
        DropRaw();Finish(Reason::OwnerChanged);
    }
    Snapshot Inspect() noexcept {
        std::lock_guard lock(mutex_);
        if(rawAvailable_ && (cancelled_.load(std::memory_order_acquire) || poisoned_.load(std::memory_order_acquire))){
            DropRaw();snapshot_.phase=enabled_?Phase::Unknown:Phase::Disabled;snapshot_.reason=Reason::OwnerChanged;
        }
        // A game may stop presenting before servicing a request. Reporting its
        // deadline needs no GPU work and never frees an in-flight resource.
        if(!quarantined_ && (requested_ || inFlight_) && GetTickCount64()-requestedAt_>=MaxRequestMilliseconds){
            if(inFlight_)Quarantine(Reason::Deadline,E_PENDING);else Finish(Reason::Deadline);
        }
        Publish();return snapshot_;
    }
    bool Read(RawEvidence& output) noexcept {
        std::lock_guard lock(mutex_);Erase(output);
        if(!enabled_ || !rawAvailable_ || cancelled_.load(std::memory_order_acquire) || poisoned_.load(std::memory_order_acquire))return false;
        output=raw_;return true;
    }
    void Service(const DrawObservationDx11::Consumer* table) noexcept {
        std::lock_guard lock(mutex_);
        // Publish under the request mutex; a stale idle result on the owner
        // thread must not unsubscribe a newly accepted UI-thread request.
        DrawObservationDx11::Subscribe(DrawObservationDx11::Slot::Uid,table,!quarantined_ && (requested_ || inFlight_));
    }
};
inline Runtime& State() {static auto* const state=new Runtime;return *state;}
} // namespace detail

inline void ObserveDraw(ID3D11DeviceContext* context,const Draw& draw,const Owner& owner,
    CapturedState& state,uint64_t& cookie) noexcept {detail::State().Before(context,draw,owner,state,cookie);}
inline void CompleteDraw(ID3D11DeviceContext* context,const Draw&,const Owner& owner,uint64_t cookie,
    DrawObservationDx11::Completion completion) noexcept {detail::State().After(context,owner,cookie,completion);}
inline void ServiceSubscription() noexcept;
inline void Present(ID3D11DeviceContext* context,const Owner& owner) noexcept {
    detail::State().Present(context,owner);ServiceSubscription();
}
inline void Revoke() noexcept {detail::State().Revoke();}
inline const DrawObservationDx11::Consumer ConsumerTable{&ObserveDraw,&CompleteDraw,&Present,&Revoke,nullptr};
inline void ServiceSubscription() noexcept {
    detail::State().Service(&ConsumerTable);
}
inline void Enable(bool enabled) noexcept {detail::State().Enable(enabled);ServiceSubscription();}
inline bool Request(const Selection& selection) noexcept {
    const bool accepted=detail::State().Request(selection);ServiceSubscription();return accepted;
}
inline Snapshot Inspect() noexcept {const auto value=detail::State().Inspect();ServiceSubscription();return value;}
// Explicit in-memory consumption only. Caller must not log or serialize bytes.
// This returns draw-bound raw data, never a decoded/authenticated UID.
inline bool ReadRawEvidence(RawEvidence& output) noexcept {return detail::State().Read(output);}
} // namespace DrawUid
