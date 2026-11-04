// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "CaptureOnlyBridge.h"
#include "GenshinFamilySkip.h"
#include <atomic>
#include <cmath>

namespace GenshinLegacy::RawObservation
{
// Observation facts have no admission, guide meaning or source-to-output contract.
enum class Space : std::uint8_t { Unknown };
enum class Encoding : std::uint8_t { Unknown };
struct Owner
{
    std::uint64_t device=0,context=0,deviceEpoch=0,producerEpoch=0,configEpoch=0,thread=0;
    bool operator==(const Owner& rhs) const noexcept
    {return device==rhs.device && context==rhs.context && deviceEpoch==rhs.deviceEpoch &&
        producerEpoch==rhs.producerEpoch && configEpoch==rhs.configEpoch && thread==rhs.thread;}
};
struct Resource
{
    std::uint64_t identity=0;
    std::uint32_t slot=0;
    D3D11_TEXTURE2D_DESC description{};
    DXGI_FORMAT srvFormat=DXGI_FORMAT_UNKNOWN;
    ResourceProof::Witness observedWrites{}; // Partial-hook witness; preserve all UNKNOWN fields.
    Space space=Space::Unknown;
    Encoding encoding=Encoding::Unknown;
};
class Capture;
class Snapshot
{
    friend class Capture;
    Snapshot()=default;
    std::atomic_bool revoked_{false};
    bool completed_=false;
    std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>,3> sourceLease_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> shaderLease_;
    bool LocallyCurrent() const noexcept
    {
        return completed_ && !revoked_.load(std::memory_order_acquire) &&
            GetCurrentThreadId()==owner.thread && token.params.instance && token.generation &&
            il2cpp_callsite::params_generation_for(token.params.instance)==token.generation;
    }
public:
    ~Snapshot()=default;
    static constexpr bool observationOnly=true;
    Owner owner{};
    il2cpp_callsite::RenderToken token{}; // Raw captured jitter/dimensions, never semantic proof.
    std::uint64_t shaderHash=0,shaderIdentity=0,shaderDeviceEpoch=0;
    std::array<Resource,3> sources{};
    std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>,3> resources;
    ResourceProof::Witness observedOutputIdentity{}; // No correspondence to any destination is asserted.
    // Re-enters the real snapshot getter, which refreshes live controls before
    // checking owner/thread/generation. Cached handles do not bypass that boundary.
    bool Current() const noexcept;
};
enum class Reason : std::uint8_t
{
    Unobserved, Pending, Completed, Revoked, WrongOwner, InvalidSource, DuplicateGeneration,
    HeldSnapshot, AllocationFailed, QueryFailed, DeviceRemoved
};
struct Receipt
{
    Reason reason=Reason::Unobserved;
    std::uint64_t submitted=0,completed=0;
    bool pending=false,current=false;
};

// Embedded in CapturePipeline::Storage: the existing retired-storage ledger
// retains this object until Poll completes. A held immutable publication applies
// backpressure; it is never overwritten or treated as downstream use.
class Capture
{
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11Query> query_;
    std::shared_ptr<Snapshot> pending_,published_;
    Receipt receipt_{};

    static bool CurrentToken(const Snapshot& snapshot) noexcept
    {return snapshot.token.params.instance && snapshot.token.generation &&
        il2cpp_callsite::params_generation_for(snapshot.token.params.instance)==snapshot.token.generation;}
public:
    ~Capture(){Revoke();}
    void Revoke() noexcept
    {
        if(pending_)pending_->revoked_.store(true,std::memory_order_release);
        if(published_)published_->revoked_.store(true,std::memory_order_release);
        receipt_.current=false;receipt_.reason=Reason::Revoked;
        // Revocation cannot release a query, source lease or copied resource.
    }
    void CheckCurrent(const Owner& owner) noexcept
    {
        if((pending_ && (!(pending_->owner==owner) || !CurrentToken(*pending_))) ||
            (published_ && (!(published_->owner==owner) || !CurrentToken(*published_))))Revoke();
    }
    bool Poll() noexcept
    {
        if(!pending_)return true;
        if(GetCurrentThreadId()!=pending_->owner.thread){receipt_.reason=Reason::WrongOwner;return false;}
        if(FAILED(device_->GetDeviceRemovedReason()))
        {
            Revoke();pending_.reset();query_.Reset();context_.Reset();device_.Reset();
            receipt_.pending=false;receipt_.reason=Reason::DeviceRemoved;return true;
        }
        BOOL complete=FALSE;
        const auto hr=context_->GetData(query_.Get(),&complete,sizeof(complete),D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if(FAILED(hr)){Revoke();receipt_.reason=Reason::QueryFailed;return false;}
        if(hr==S_FALSE || !complete)return false;
        query_.Reset();receipt_.pending=false;++receipt_.completed;
        pending_->completed_=true;
        if(!pending_->revoked_.load(std::memory_order_acquire) && CurrentToken(*pending_))
        {published_=std::move(pending_);receipt_.reason=Reason::Completed;receipt_.current=true;}
        else{pending_.reset();receipt_.reason=Reason::Revoked;receipt_.current=false;}
        context_.Reset();device_.Reset();return true;
    }
    bool Begin(const Owner& owner,const il2cpp_callsite::RenderToken& token,const CaptureSource& source) noexcept
    {
        CheckCurrent(owner);
        if(pending_){receipt_.reason=Reason::Pending;return false;}
        if(!source.device || !source.context || !owner.device || !owner.context || !owner.deviceEpoch ||
            !owner.producerEpoch || !owner.configEpoch || !owner.thread || GetCurrentThreadId()!=owner.thread ||
            reinterpret_cast<std::uintptr_t>(source.device)!=owner.device ||
            reinterpret_cast<std::uintptr_t>(source.context)!=owner.context || source.deviceEpoch!=owner.deviceEpoch ||
            source.ownerEpoch!=owner.producerEpoch || source.ownerThread!=owner.thread ||
            source.context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)
        {receipt_.reason=Reason::WrongOwner;return false;}
        const auto& params=token.params;
        if(!params.instance || !params.frame_index || !token.generation || !token.capture_tick ||
            !params.render_w || !params.render_h || !params.display_w || !params.display_h ||
            !std::isfinite(params.jitter_x) || !std::isfinite(params.jitter_y) ||
            il2cpp_callsite::params_generation_for(params.instance)!=token.generation ||
            !source.shaderHash || !source.shaderIdentity || source.shaderDeviceEpoch!=owner.deviceEpoch)
        {receipt_.reason=Reason::InvalidSource;return false;}
        if(published_ && published_->token.params.instance==params.instance && published_->token.generation==token.generation)
        {receipt_.reason=Reason::DuplicateGeneration;return false;}
        if(published_)published_->revoked_.store(true,std::memory_order_release);
        if(published_ && published_.use_count()!=1)
        {receipt_.reason=Reason::HeldSnapshot;return false;}
        std::shared_ptr<Snapshot> next;
        try{next=std::shared_ptr<Snapshot>(new Snapshot());}catch(...){receipt_.reason=Reason::AllocationFailed;return false;}
        next->owner=owner;next->token=token;next->shaderHash=source.shaderHash;
        next->shaderIdentity=source.shaderIdentity;next->shaderDeviceEpoch=source.shaderDeviceEpoch;
        next->observedOutputIdentity=source.outputIdentity;
        Microsoft::WRL::ComPtr<ID3D11Device> contextDevice;source.context->GetDevice(&contextDevice);
        if(contextDevice.Get()!=source.device){receipt_.reason=Reason::WrongOwner;return false;}
        source.context->PSGetShader(&next->shaderLease_,nullptr,nullptr);
        const auto shader=Family::Read(source.device,next->shaderLease_.Get(),owner.deviceEpoch);
        if(!shader.Known() || shader.hash!=fsr2_family_takeover::k_accumulate_hash ||
            shader.hash!=source.shaderHash || shader.identity!=source.shaderIdentity || shader.deviceEpoch!=source.shaderDeviceEpoch)
        {receipt_.reason=Reason::InvalidSource;return false;}
        for(unsigned i=0;i<3;++i)
        {
            if(!source.resources[i] || source.viewFormats[i]==DXGI_FORMAT_UNKNOWN)
            {receipt_.reason=Reason::InvalidSource;return false;}
            Microsoft::WRL::ComPtr<ID3D11Device> resourceDevice;source.resources[i]->GetDevice(&resourceDevice);
            if(resourceDevice.Get()!=source.device){receipt_.reason=Reason::WrongOwner;return false;}
            auto& fact=next->sources[i];fact.identity=reinterpret_cast<std::uintptr_t>(source.resources[i]);
            fact.slot=source.srvSlots[i];fact.srvFormat=source.viewFormats[i];fact.observedWrites=source.sourceWrites[i];
            source.resources[i]->GetDesc(&fact.description);
            const auto& actual=fact.description;
            if(actual.Width!=params.render_w || actual.Height!=params.render_h || actual.MipLevels!=1 ||
                actual.ArraySize!=1 || actual.SampleDesc.Count!=1 || actual.SampleDesc.Quality ||
                actual.Usage!=D3D11_USAGE_DEFAULT || actual.CPUAccessFlags || actual.MiscFlags)
            {receipt_.reason=Reason::InvalidSource;return false;}
            auto copyDescription=actual;copyDescription.BindFlags=0;copyDescription.MiscFlags=0;
            if(FAILED(source.device->CreateTexture2D(&copyDescription,nullptr,&next->resources[i])))
            {receipt_.reason=Reason::AllocationFailed;return false;}
            next->sourceLease_[i]=source.resources[i];
        }
        D3D11_QUERY_DESC description{D3D11_QUERY_EVENT,0};
        Microsoft::WRL::ComPtr<ID3D11Query> query;
        if(FAILED(source.device->CreateQuery(&description,&query)))
        {receipt_.reason=Reason::QueryFailed;return false;}
        if(FAILED(source.device->GetDeviceRemovedReason()))
        {Revoke();receipt_.reason=Reason::DeviceRemoved;return false;}
        // All allocations and metadata are committed before the first submitted copy.
        published_.reset();pending_=std::move(next);
        device_=source.device;context_=source.context;query_=std::move(query);
        receipt_.pending=true;receipt_.current=false;receipt_.reason=Reason::Pending;++receipt_.submitted;
        for(unsigned i=0;i<3;++i)context_->CopyResource(pending_->resources[i].Get(),pending_->sourceLease_[i].Get());
        context_->End(query_.Get());return true;
    }
    std::uint64_t Instance() const noexcept
    {return pending_?pending_->token.params.instance:(published_?published_->token.params.instance:0);}
    std::shared_ptr<const Snapshot> Get() const noexcept
    {return !pending_ && published_ && published_->LocallyCurrent()?published_:nullptr;}
    Receipt GetReceipt() const noexcept
    {auto result=receipt_;result.current=static_cast<bool>(Get());return result;}
};

// Only this separate observation type is exposed; CaptureSnapshot admission is untouched.
std::shared_ptr<const Snapshot> GetSnapshot() noexcept;
Receipt GetReceipt() noexcept;
}
