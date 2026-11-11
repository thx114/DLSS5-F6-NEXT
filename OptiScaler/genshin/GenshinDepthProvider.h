// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <utility>

namespace GenshinLegacy::NativeMrt { struct Frame; }

namespace GenshinDepthProvider {
// Preview only: no depth convention, NR/FG eligibility or final-picture mapping.
void CaptureClaimed(ID3D11DeviceContext*, const GenshinLegacy::NativeMrt::Frame&,
                    ID3D11ShaderResourceView*) noexcept;
void RevokePreview() noexcept;
void DrawPreview();
// Called by the actual DX11 menu renderer around synchronous ImGui recording.
void BeginPreviewDraw(ID3D11DeviceContext*) noexcept;
void EndPreviewDraw(ID3D11DeviceContext*, bool recorded) noexcept;
void ClearDepth() noexcept;

// Actual preview demand/ownership only, never SR/resource admission. Querying
// does not instantiate Storage, lock, scan, Poll, export or retire any owner.
struct ObservationDemand {
    bool known=false,requested=false,pending=false,draining=false;
    bool Idle() const noexcept {return known && !requested && !pending && !draining;}
};
ObservationDemand ReadObservationDemand() noexcept;

namespace detail {
using Microsoft::WRL::ComPtr;
inline bool Same(IUnknown* a, IUnknown* b) noexcept {
    ComPtr<IUnknown> x, y;
    return a && b && SUCCEEDED(a->QueryInterface(IID_PPV_ARGS(&x))) &&
        SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(&y))) && x.Get() == y.Get();
}
struct Stamp {
    uint64_t generation=0, sourceFrame=0, deviceEpoch=0, ownerEpoch=0, configEpoch=0, instance=0;
    DWORD thread=0;
};
// Shared by the product and offline GPU fixture. Callers serialize access.
// Exactly three reusable allocations; never overwrite a copy or menu read in flight.
class RawPreviewRing final {
public:
    struct Slot {
        ComPtr<ID3D11DeviceContext> context;
        ComPtr<ID3D11Texture2D> source, texture;
        ComPtr<ID3D11ShaderResourceView> view;
        ComPtr<ID3D11Query> copied, read;
        Stamp stamp{};
        uint64_t serial=0, tick=0;
        UINT width=0, height=0;
        DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN, viewFormat=DXGI_FORMAT_UNKNOWN;
        bool copyPending=false, readPending=false, leased=false, visible=false;
    };
private:
    std::array<Slot,3> slots_{};
    uint64_t serial_=0;
    const char* failure_="Waiting for a copy.";
    // Each low bit is a REAL copy/read/lease flag in one of the three slots.
    // Cached visibility/resources alone do not publish an active reader.
    static constexpr uint32_t PendingMask=0x1ffu,ChangingBit=1u<<30,UnknownBit=1u<<31;
    std::atomic<uint32_t> observation_{0};
    uint32_t Bit(const Slot& slot,unsigned channel)const noexcept {
        return 1u<<(static_cast<unsigned>(&slot-slots_.data())*3+channel);
    }
    void Publish(const Slot& slot,unsigned channel,bool active)noexcept {
        const auto bit=Bit(slot,channel);
        if(active)observation_.fetch_or(bit,std::memory_order_acq_rel);
        else observation_.fetch_and(~bit,std::memory_order_acq_rel);
    }
    // Only Capture/Acquire can create work from an idle ring. Publish their
    // mutation interval before any validation/COM work; Poll is clear-only.
    struct Activity {
        RawPreviewRing& ring;
        explicit Activity(RawPreviewRing& value)noexcept:ring(value) {
            if(ring.observation_.fetch_or(ChangingBit,std::memory_order_acq_rel)&ChangingBit)
                ring.MarkObservationUnknown();
        }
        ~Activity(){ring.observation_.fetch_and(~ChangingBit,std::memory_order_acq_rel);}
        Activity(const Activity&)=delete;
        Activity& operator=(const Activity&)=delete;
    };
    static bool Complete(ID3D11DeviceContext* context, ID3D11Query* query) noexcept {
        BOOL done=FALSE;
        return context && query && context->GetData(query,&done,sizeof(done),D3D11_ASYNC_GETDATA_DONOTFLUSH)==S_OK && done;
    }
public:
    ObservationDemand ReadOwnershipDemand()const noexcept {
        const auto state=observation_.load(std::memory_order_acquire);
        return {!(state&(ChangingBit|UnknownBit)),false,(state&PendingMask)!=0,false};
    }
    void MarkObservationUnknown()noexcept {observation_.fetch_or(UnknownBit,std::memory_order_acq_rel);}
    void Poll() noexcept {
        for(auto& s:slots_) {
            if(!s.context || s.stamp.thread!=GetCurrentThreadId())continue;
            if(s.copyPending && Complete(s.context.Get(),s.copied.Get())) {
                s.copyPending=false; s.source.Reset();Publish(s,0,false);
            }
            if(s.readPending && Complete(s.context.Get(),s.read.Get())){s.readPending=false;Publish(s,1,false);}
            if(!s.visible && !s.copyPending && !s.readPending && !s.leased)s=Slot{};
        }
    }
    void Revoke() noexcept { for(auto& s:slots_)s.visible=false; Poll(); }
    bool Capture(ID3D11DeviceContext* context, ID3D11Texture2D* source,
                 DXGI_FORMAT viewFormat, UINT mip, const Stamp& stamp) noexcept {
        Activity observation(*this);
        failure_="Wrong immediate context, thread or frame.";
        if(!context || !source || context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE ||
           stamp.thread!=GetCurrentThreadId() || !stamp.generation || !stamp.sourceFrame)return false;
        // CopySubresourceRegion honors predication. An EVENT alone cannot prove
        // a predicated copy wrote anything; never change the game's predicate.
        ComPtr<ID3D11Predicate> predicate;BOOL predicateValue=FALSE;
        context->GetPredication(&predicate,&predicateValue);
        if(predicate){failure_="Game predication is bound; preview copy skipped.";return false;}
        ComPtr<ID3D11Device> device, owner; context->GetDevice(&device); source->GetDevice(&owner);
        failure_="Wrong source device or removed device.";
        if(!Same(device.Get(),owner.Get()) || FAILED(device->GetDeviceRemovedReason()))return false;
        D3D11_TEXTURE2D_DESC desc{};source->GetDesc(&desc);
        const bool readable=(viewFormat==DXGI_FORMAT_R32_FLOAT &&
            (desc.Format==DXGI_FORMAT_R32_FLOAT || desc.Format==DXGI_FORMAT_R32_TYPELESS)) ||
            (viewFormat==DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS && desc.Format==DXGI_FORMAT_R32G8X24_TYPELESS);
        failure_="Unsupported depth plane or subresource layout.";
        if(!readable || desc.ArraySize!=1 || desc.SampleDesc.Count!=1 || desc.SampleDesc.Quality ||
           !desc.Width || !desc.Height || mip>=desc.MipLevels || mip>=D3D11_REQ_MIP_LEVELS)return false;
        const UINT width=(desc.Width>>mip)?(desc.Width>>mip):1, height=(desc.Height>>mip)?(desc.Height>>mip):1;
        if(uint64_t(width)*height*8>64ull*1024*1024){failure_="Preview memory budget exceeded.";return false;}
        Poll(); Slot* target=nullptr;
        for(auto& s:slots_)if(!s.copyPending && !s.readPending && !s.leased &&
            (!target || s.serial<target->serial))target=&s;
        if(!target || serial_==UINT64_MAX){failure_="Preview slots busy; keeping in-flight snapshots.";return false;}
        auto& s=*target;
        if(!s.texture || !Same(s.context.Get(),context) || s.width!=width || s.height!=height ||
           s.format!=desc.Format || s.viewFormat!=viewFormat) {
            Slot next;next.context=context;next.width=width;next.height=height;next.format=desc.Format;next.viewFormat=viewFormat;
            D3D11_TEXTURE2D_DESC clone{};clone.Width=width;clone.Height=height;clone.MipLevels=1;clone.ArraySize=1;
            clone.Format=desc.Format;clone.SampleDesc.Count=1;clone.Usage=D3D11_USAGE_DEFAULT;clone.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            D3D11_SHADER_RESOURCE_VIEW_DESC view{};view.Format=viewFormat;view.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;view.Texture2D.MipLevels=1;
            const D3D11_QUERY_DESC query{D3D11_QUERY_EVENT,0};
            failure_="Preview texture, typed view or query creation failed.";
            if(FAILED(device->CreateTexture2D(&clone,nullptr,&next.texture)) ||
               FAILED(device->CreateShaderResourceView(next.texture.Get(),&view,&next.view)) ||
               FAILED(device->CreateQuery(&query,&next.copied)) || FAILED(device->CreateQuery(&query,&next.read)))return false;
            s=std::move(next);
        }
        Publish(s,0,true); // Retain Full before publishing/issuing the real GPU copy.
        s.source=source;s.stamp=stamp;s.serial=++serial_;s.tick=GetTickCount64();s.visible=true;s.copyPending=true;
        context->CopySubresourceRegion(s.texture.Get(),0,0,0,0,source,mip,nullptr);
        context->End(s.copied.Get());failure_="Waiting for GPU snapshot completion.";return true;
    }
    Slot* Acquire(ID3D11DeviceContext* context) noexcept {
        Activity observation(*this);
        Poll();Slot* newest=nullptr;const auto now=GetTickCount64();
        for(auto& s:slots_)if(s.visible && !s.copyPending && !s.readPending && !s.leased &&
            s.stamp.thread==GetCurrentThreadId() && Same(s.context.Get(),context) && now>=s.tick && now-s.tick<=500 &&
            (!newest || s.serial>newest->serial))newest=&s;
        if(newest){Publish(*newest,2,true);newest->leased=true;}
        return newest;
    }
    void Finish(ID3D11DeviceContext* context,bool recorded) noexcept {
        for(auto& s:slots_)if(s.leased && s.stamp.thread==GetCurrentThreadId() && Same(s.context.Get(),context)) {
            // The query is issued AFTER this exact backend recorded the image read.
            if(recorded){Publish(s,1,true);s.readPending=true;context->End(s.read.Get());}
            s.leased=false;Publish(s,2,false); // Transfer to readPending without a zero-reader gap.
        }
        Poll();
    }
    const std::array<Slot,3>& Slots()const noexcept{return slots_;}
    const char* LastFailure()const noexcept{return failure_;}
};
}

// Kept for binary compatibility only. The old boolean cannot represent unknown
// convention, so unverified raw previews are never exported through this ABI.
struct OptiScalerDepthDx11Frame {
    ID3D11Device* device=nullptr; ID3D11Texture2D* texture=nullptr;
    uint32_t width=0,height=0,format=0;bool depth_inverted=false;
};
extern "C" {
#if !defined(GENSHIN_DEPTH_PREVIEW_RING_ONLY)
__declspec(dllexport) BOOL WINAPI OptiScalerGetDepthDx11Frame(OptiScalerDepthDx11Frame*);
__declspec(dllexport) void WINAPI OptiScalerReleaseDepthDx11Frame(OptiScalerDepthDx11Frame*);
#endif
}
}
