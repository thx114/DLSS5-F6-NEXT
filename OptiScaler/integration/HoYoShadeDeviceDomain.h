#pragma once
#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>

namespace HoYoShade::DeviceDomain
{
// This GUID belongs only to this Opt integration. Its data is POD, never an
// IUnknown or a pointer. No COM vtable can outlive this module through the tag.
inline constexpr GUID MarkerGuid{0x975876c1,0xcf4b,0x4235,{0x89,0xb5,0x96,0x70,0x52,0x3a,0xf1,0x6c}};
struct Marker {
    uint64_t serial=0, nonce=0;
    bool operator==(const Marker&) const = default;
    explicit operator bool() const noexcept { return serial && nonce; }
};
enum class Result : unsigned { Deferred, Missing, Accepted, Nested, CallbackMismatch,
    CreateFailed, ReturnedMismatch, Exception, MarkerForeign, MarkerIo, Capacity, LifetimeChanged };
inline const char* Text(Result v) noexcept {
    constexpr const char* names[]{"deferred-not-ready","callback-missing","accepted-public-rtv-callback",
        "nested-capture","callback-mismatch","typed-create-failed","typed-return-mismatch","probe-exception",
        "marker-foreign-or-replaced","marker-read-or-write-failed","bounded-registry-full","runtime-lifetime-changed"};
    return static_cast<unsigned>(v)<sizeof(names)/sizeof(*names)?names[static_cast<unsigned>(v)]:"unknown";
}
struct Witness {
    Result result=Result::Deferred;
    uint64_t stamp=0, sdkDevice=0, nativeDevice=0, resource=0, view=0;
    uint64_t creatorDevice=0, returnedOwner=0;
    // Adoption pin, refreshed from the same live lifetime on a cache hit.
    uint64_t runtime=0, lifetime=0, epoch=0;
    DWORD thread=0;
    unsigned api=0, callbacks=0;
    int32_t createHr=E_PENDING;
};
// Receipt for this ONE actual typed RTV return. A public creator and the
// returned view's public owner need not be pointer aliases. This receipt never
// authorizes any other view, device, queue, list or GPU submission.
class TypedRtv final {
    Microsoft::WRL::ComPtr<ID3D11Device> creator_, returnedOwner_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> buffer_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> view_;
    bool attempted_=false;
    int32_t hr_=E_PENDING;
    static uint64_t Key(const void* p) noexcept {return reinterpret_cast<uintptr_t>(p);}
public:
    TypedRtv()=default;
    TypedRtv(const TypedRtv&)=delete;
    TypedRtv& operator=(const TypedRtv&)=delete;
    int32_t Create(ID3D11Device* creator,ID3D11Texture2D* buffer) {
        if(attempted_ || !creator || !buffer)return E_UNEXPECTED;
        attempted_=true;creator_=creator;buffer_=buffer;
        ID3D11RenderTargetView* returned=nullptr;
        hr_=creator_->CreateRenderTargetView(buffer_.Get(),nullptr,&returned);
        // A failed Create has no usable COM return, even when non-null.
        if(hr_<0 || !returned)return hr_;
        view_.Attach(returned);view_->GetDevice(&returnedOwner_);
        return hr_;
    }
    uint64_t View() const noexcept {return Key(view_.Get());}
    uint64_t Creator() const noexcept {return Key(creator_.Get());}
    uint64_t ReturnedOwner() const noexcept {return Key(returnedOwner_.Get());}
    bool Current(uint64_t creator,uint64_t resource) const {
        if(!attempted_ || hr_<0 || !creator_ || !buffer_ || !view_ || !returnedOwner_ ||
            creator!=Creator() || resource!=Key(buffer_.Get()))return false;
        Microsoft::WRL::ComPtr<ID3D11Device> owner,bufferOwner;
        Microsoft::WRL::ComPtr<ID3D11Resource> returned;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        view_->GetDevice(&owner);view_->GetResource(&returned);
        if(returned)returned.As(&texture);
        buffer_->GetDevice(&bufferOwner);
        return owner && owner.Get()==returnedOwner_.Get() && texture.Get()==buffer_.Get() &&
            bufferOwner.Get()==creator_.Get() && SUCCEEDED(creator_->GetDeviceRemovedReason()) &&
            SUCCEEDED(returnedOwner_->GetDeviceRemovedReason());
    }
};
// One TLS interval around a real typed CreateRenderTargetView, never around a
// scalar SDK handle. Nesting poisons the outer interval and cannot replace it.
class Capture final {
    inline static thread_local Capture* current_=nullptr;
    bool acquired_=false, inside_=false;
public:
    Witness value;
    Capture(uint64_t stamp, uint64_t resource) noexcept {
        value.stamp=stamp;value.resource=resource;value.thread=GetCurrentThreadId();value.result=Result::Missing;
        if(current_) { current_->Fail(Result::Nested);value.result=Result::Nested;return; }
        if(!stamp || !resource)return;
        current_=this;acquired_=true;
    }
    ~Capture(){if(acquired_)current_=nullptr;}
    Capture(const Capture&)=delete;
    Capture& operator=(const Capture&)=delete;
    static Capture* Current() noexcept {return current_;}
    bool Acquired() const noexcept {return acquired_;}
    void Fail(Result why) noexcept {value.result=why;}
    bool BeginCallback() noexcept {
        if(!acquired_ || inside_ || value.thread!=GetCurrentThreadId() || value.callbacks++) {
            Fail(Result::Nested);return false;
        }
        inside_=true;return true;
    }
    void EndCallback(unsigned api,uint64_t sdk,uint64_t native,uint64_t resource,uint64_t view) noexcept {
        inside_=false;
        if(value.result!=Result::Missing)return;
        if(api!=0xb000 || !sdk || !native || !view || resource!=value.resource) {
            Fail(Result::CallbackMismatch);return;
        }
        value.api=api;value.sdkDevice=sdk;value.nativeDevice=native;value.view=view;
    }
    Witness Returned(int32_t hr,const TypedRtv& returned,uint64_t creator) {
        value.createHr=hr;
        if(value.result!=Result::Missing)return value;
        if(hr<0 || !returned.View()) {Fail(Result::CreateFailed);return value;}
        if(value.callbacks!=1 || !value.sdkDevice || !value.nativeDevice)return value;
        if(inside_ || value.thread!=GetCurrentThreadId() || returned.View()!=value.view ||
            !returned.Current(creator,value.resource)) {Fail(Result::ReturnedMismatch);return value;}
        // Current performs public COM calls: reentrant poison is sticky.
        if(value.result!=Result::Missing)return value;
        value.creatorDevice=returned.Creator();value.returnedOwner=returned.ReturnedOwner();
        value.result=Result::Accepted;return value;
    }
};
inline bool Matches(const Witness& w,unsigned api,uint64_t sdk,uint64_t native,uint64_t resource,uint64_t creator) noexcept {
    return w.result==Result::Accepted && w.stamp && w.callbacks==1 && w.api==0xb000 && api==w.api &&
        sdk && sdk==w.sdkDevice && native && native==w.nativeDevice && resource && resource==w.resource &&
        creator && creator==w.creatorDevice && w.returnedOwner;
}

enum class MarkerRead { Absent, KnownShape, Invalid };
// Only fixed-size bytes are read. An alien/short/interface-valued property is
// rejected by size and registry lookup, with no call through its contents.
template<class Resource> MarkerRead ReadMarker(Resource* resource,Marker& value) noexcept {
    value={};UINT size=0;
    const auto hr=resource->GetPrivateData(MarkerGuid,&size,nullptr);
    if(hr==DXGI_ERROR_NOT_FOUND)return MarkerRead::Absent;
    if(hr!=S_OK || size!=sizeof(value))return MarkerRead::Invalid;
    size=sizeof(value);
    if(resource->GetPrivateData(MarkerGuid,&size,&value)!=S_OK || size!=sizeof(value) || !value)
        return MarkerRead::Invalid;
    return MarkerRead::KnownShape;
}
}
