// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d11_1.h>
#include <wrl/client.h>
#include <atomic>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace taa::constants {
using Microsoft::WRL::ComPtr;
inline constexpr GUID ShadowKey = {0xf6aa0001,0x928b,0x4cb7,{0xa5,0x91,0xda,0x11,0x71,0x21,0x83,0x05}};
inline std::atomic<uint64_t> epoch{1};
inline std::atomic<uint64_t> writes{0},geometryEpoch{1};
inline constexpr GUID DomainKey={0xf6aa0005,0x928b,0x4cb7,{0xa5,0x91,0xda,0x11,0x71,0x21,0x83,0x05}};
// An unknown implementation may write buffers belonging to its own device.
// Auxiliary overlay/provider devices cannot revoke a game's immutable history
// of CPU writes. The device owns this domain and every shadow retains it.
class Domain final : public IUnknown {
    std::atomic<ULONG> refs_{1};
public:
    std::atomic<uint64_t> revision{0};
    std::atomic_uint geometrySubmissions{0};
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override {if(!out)return E_POINTER;*out=nullptr;if(id!=__uuidof(IUnknown))return E_NOINTERFACE;*out=static_cast<IUnknown*>(this);AddRef();return S_OK;}
    ULONG STDMETHODCALLTYPE AddRef() override{return ++refs_;}
    ULONG STDMETHODCALLTYPE Release() override{const auto count=--refs_;if(!count)delete this;return count;}
};
inline ComPtr<Domain> DeviceDomain(ID3D11Device* device) {
    if(!device)return {};
    const auto find=[&]{ComPtr<Domain> d;UINT size=sizeof(IUnknown*);IUnknown* object=nullptr;if(SUCCEEDED(device->GetPrivateData(DomainKey,&size,&object))&&object)d.Attach(static_cast<Domain*>(object));return d;};
    if(auto d=find())return d;
    static std::mutex creation;std::lock_guard lock(creation);
    if(auto d=find())return d;
    ComPtr<Domain> d;d.Attach(new Domain());if(FAILED(device->SetPrivateDataInterface(DomainKey,d.Get())))return {};return d;
}
// Opaque deferred submission can target any geometry buffer. Keep the whole
// driver submission inside the domain boundary, including recursive execution.
class GeometrySubmission final {
    ComPtr<Domain> domain_;
public:
    explicit GeometrySubmission(ID3D11DeviceContext* context) {
        ComPtr<ID3D11Device> device;if(context)context->GetDevice(&device);
        domain_=DeviceDomain(device.Get());if(!domain_)return;
        domain_->geometrySubmissions.fetch_add(1,std::memory_order_acq_rel);
        domain_->revision.fetch_add(1,std::memory_order_acq_rel);
    }
    ~GeometrySubmission(){if(domain_){domain_->revision.fetch_add(1,std::memory_order_acq_rel);domain_->geometrySubmissions.fetch_sub(1,std::memory_order_acq_rel);}}
    GeometrySubmission(const GeometrySubmission&)=delete;
    GeometrySubmission& operator=(const GeometrySubmission&)=delete;
};
inline uint64_t ConstantEpoch(ID3D11DeviceContext* context) {
    ComPtr<ID3D11Device> device;if(context)context->GetDevice(&device);
    const auto d=DeviceDomain(device.Get());return epoch.load(std::memory_order_acquire)+(d?d->revision.load(std::memory_order_acquire):0);
}
// CPU content mirror for explicitly selected constant buffers and small
// fullscreen geometry. The resource owns it; no pointer-keyed resource cache.
class Shadow final : public IUnknown {
    std::atomic<ULONG> refs_{1};
public:
    std::mutex mutex;
    std::vector<unsigned char> bytes, known;
    uint64_t generation = 0, writeSerial = 0, revision = 0;
    const void* mapped = nullptr;
    ID3D11DeviceContext* mapper = nullptr; // borrowed until the matching Unmap
    bool pending = false;
    bool geometry=false,immutable=false;
    bool recoveryMode=false,targetedWrite=false,initialContentsObserved=false;
    ComPtr<Domain> domain;
    explicit Shadow(unsigned size,bool g=false,bool i=false,ComPtr<Domain> d={}) : bytes(size), known(size),geometry(g),immutable(i),domain(std::move(d)) {}
    uint64_t CurrentEpoch()const {return immutable?0:(geometry?geometryEpoch:epoch).load(std::memory_order_acquire)+(domain?domain->revision.load(std::memory_order_acquire):0);}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id != __uuidof(IUnknown)) return E_NOINTERFACE;
        *out = static_cast<IUnknown*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto refs = --refs_; if (!refs) delete this; return refs; }
    void Invalidate() { ++revision; generation = 0; writeSerial = 0; std::fill(known.begin(), known.end(), static_cast<unsigned char>(0)); mapped = nullptr; mapper = nullptr; pending = false; }
};
inline ComPtr<Shadow> Find(ID3D11Resource* resource) {
    ComPtr<Shadow> result; UINT size = sizeof(IUnknown*); IUnknown* object = nullptr;
    if (resource && SUCCEEDED(resource->GetPrivateData(ShadowKey, &size, &object)) && object) result.Attach(static_cast<Shadow*>(object));
    return result;
}
inline ComPtr<Shadow> Observe(ID3D11Buffer* buffer) {
    if (!buffer) return {};
    auto shadow = Find(buffer);
    if (!shadow) {
        D3D11_BUFFER_DESC desc{}; buffer->GetDesc(&desc);
        const bool constant=(desc.BindFlags&D3D11_BIND_CONSTANT_BUFFER)&&desc.ByteWidth<=65536;
        constexpr UINT geometryBindings=D3D11_BIND_VERTEX_BUFFER|D3D11_BIND_INDEX_BUFFER;
        const bool geometry=(desc.BindFlags&geometryBindings)&&!(desc.BindFlags&~geometryBindings)&&!desc.MiscFlags&&desc.ByteWidth<=4096;
        if ((!constant&&!geometry)||!desc.ByteWidth) return {};
        ComPtr<ID3D11Device> device;buffer->GetDevice(&device);
        auto domain=DeviceDomain(device.Get());if(!domain)return {};
        shadow.Attach(new Shadow(desc.ByteWidth,!constant,geometry&&desc.Usage==D3D11_USAGE_IMMUTABLE,std::move(domain)));
        if (FAILED(buffer->SetPrivateDataInterface(ShadowKey, shadow.Get()))) return {};
    }
    return shadow;
}
// Recovery admits only static geometry. Mark before the driver call, including
// writes that are blocked behind the owner thread's registry lock. This sticky
// fact cannot be cleared by a later CPU update or by finishing an old readback.
inline void GeometryWrite(ID3D11Resource* resource) {
    if(auto shadow=Find(resource)){std::lock_guard lock(shadow->mutex);if(shadow->geometry)shadow->targetedWrite=true;}
}
inline bool RecoveryValid(const Shadow& shadow) {
    return !shadow.recoveryMode||(!shadow.targetedWrite&&(!shadow.domain||!shadow.domain->geometrySubmissions.load(std::memory_order_acquire)));
}
inline void Initialized(ID3D11Buffer* buffer,const D3D11_SUBRESOURCE_DATA* data) {
    if(!buffer||!data||!data->pSysMem)return;
    auto shadow=Observe(buffer);if(!shadow)return;
    std::lock_guard lock(shadow->mutex);std::memcpy(shadow->bytes.data(),data->pSysMem,shadow->bytes.size());
    std::fill(shadow->known.begin(),shadow->known.end(),static_cast<unsigned char>(1));shadow->generation=shadow->CurrentEpoch();shadow->writeSerial=++writes;++shadow->revision;shadow->initialContentsObserved=true;
}
struct ReadbackStamp {uint64_t epoch=0,revision=0;};
inline bool StampReadback(ID3D11Buffer* buffer,ReadbackStamp& stamp) {
    auto shadow=Observe(buffer);if(!shadow)return false;
    std::lock_guard lock(shadow->mutex);
    if(!shadow->geometry||!shadow->initialContentsObserved||shadow->targetedWrite||shadow->mapped||shadow->pending||(shadow->domain&&shadow->domain->geometrySubmissions.load(std::memory_order_acquire)))return false;
    shadow->recoveryMode=true;
    stamp={shadow->CurrentEpoch(),shadow->revision};return true;
}
inline bool PublishReadback(ID3D11Buffer* buffer,const ReadbackStamp& stamp,const void* bytes,size_t size) {
    auto shadow=Find(buffer);if(!shadow||!bytes)return false;
    std::lock_guard lock(shadow->mutex);
    if(!RecoveryValid(*shadow.Get())||shadow->mapped||shadow->pending||shadow->CurrentEpoch()!=stamp.epoch||shadow->revision!=stamp.revision||shadow->bytes.size()!=size)return false;
    std::memcpy(shadow->bytes.data(),bytes,size);
    std::fill(shadow->known.begin(),shadow->known.end(),static_cast<unsigned char>(1));
    shadow->generation=stamp.epoch;shadow->writeSerial=++writes;++shadow->revision;return true;
}
inline bool Read(ID3D11Buffer* buffer, unsigned offset, float& value) {
    auto shadow=Observe(buffer);if(!shadow)return false;
    std::lock_guard lock(shadow->mutex);
    if (!RecoveryValid(*shadow.Get()) || shadow->mapped || shadow->pending || shadow->generation != shadow->CurrentEpoch() ||
        offset > shadow->bytes.size() || shadow->bytes.size() - offset < sizeof(float)) return false;
    for (unsigned i = 0; i < sizeof(float); ++i) if (!shadow->known[offset+i]) return false;
    std::memcpy(&value, shadow->bytes.data() + offset, sizeof(float)); return true;
}
// CPU-only evidence. Unknown/unobserved bytes are explicitly masked, never
// interpreted as zero-valued constants or permission to run an upscaler.
inline bool Snapshot(ID3D11Buffer* buffer,unsigned offset,unsigned count,
    std::vector<unsigned char>& bytes,std::vector<unsigned char>& known,uint64_t* writeSerial=nullptr,uint64_t* generation=nullptr) {
    bytes.assign(count,0);known.assign(count,0);
    if(writeSerial)*writeSerial=0;
    if(generation)*generation=0;
    auto shadow=Observe(buffer);if(!shadow)return false;
    std::lock_guard lock(shadow->mutex);
    if(!RecoveryValid(*shadow.Get())||shadow->mapped||shadow->pending||shadow->generation!=shadow->CurrentEpoch()||
        offset>shadow->bytes.size()||count>shadow->bytes.size()-offset)return false;
    std::copy_n(shadow->known.begin()+offset,count,known.begin());
    for(unsigned i=0;i<count;++i)if(known[i])bytes[i]=shadow->bytes[offset+i];
    if(writeSerial)*writeSerial=shadow->writeSerial;
    if(generation)*generation=shadow->generation;
    return true;
}
inline void Written(ID3D11Resource* resource) {
    if (auto shadow = Find(resource)) { std::lock_guard lock(shadow->mutex); if(shadow->geometry)shadow->targetedWrite=true;shadow->Invalidate(); }
}
inline void InvalidateConstants() {epoch.fetch_add(1,std::memory_order_acq_rel);}
inline void OpaqueWrite(ID3D11DeviceContext* context=nullptr) {
    if(context){ComPtr<ID3D11Device> device;context->GetDevice(&device);if(auto d=DeviceDomain(device.Get())){d->revision.fetch_add(1,std::memory_order_acq_rel);return;}}
    InvalidateConstants();geometryEpoch.fetch_add(1,std::memory_order_acq_rel);
}
inline void Mapped(ID3D11DeviceContext* context, ID3D11Resource* resource, UINT sub, D3D11_MAP type,
    const D3D11_MAPPED_SUBRESOURCE* data, HRESULT hr) {
    if (FAILED(hr)) return;
    if (auto shadow = Find(resource)) {
        std::lock_guard lock(shadow->mutex); shadow->Invalidate();
        if (!sub && type != D3D11_MAP_READ && context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE && data)
            { shadow->mapped = data->pData; shadow->mapper = context; }
    }
}
inline void BeforeUnmap(ID3D11DeviceContext* context, ID3D11Resource* resource, UINT sub) {
    if (auto shadow = Find(resource)) {
        std::lock_guard lock(shadow->mutex);
        if (!sub && shadow->mapped && shadow->mapper == context) {
            std::memcpy(shadow->bytes.data(), shadow->mapped, shadow->bytes.size());
            shadow->mapped = nullptr; shadow->pending = true;
        } else shadow->Invalidate();
    }
}
inline void AfterUnmap(ID3D11DeviceContext* context, ID3D11Resource* resource, UINT sub) {
    if (auto shadow = Find(resource)) {
        std::lock_guard lock(shadow->mutex);
        if (!sub && shadow->pending && shadow->mapper == context) {
            std::fill(shadow->known.begin(), shadow->known.end(), static_cast<unsigned char>(1));
            shadow->pending = false; shadow->mapper = nullptr; shadow->generation = shadow->CurrentEpoch();shadow->writeSerial=++writes;++shadow->revision;
        }
    }
}
inline void Updated(ID3D11DeviceContext* context, ID3D11Resource* resource, UINT sub, const D3D11_BOX* box, const void* data, UINT flags=0) {
    if (auto shadow = Find(resource)) {
        std::lock_guard lock(shadow->mutex);
        if (sub || !data || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) { shadow->Invalidate(); return; }
        const size_t start = box ? box->left : 0, end = box ? box->right : shadow->bytes.size();
        if (start >= end || end > shadow->bytes.size() || (box && (box->top || box->front || box->bottom != 1 || box->back != 1))) {
            shadow->Invalidate(); return;
        }
        const auto current = shadow->CurrentEpoch();
        if (shadow->generation != current || (flags&D3D11_COPY_DISCARD)) shadow->Invalidate();
        std::memcpy(shadow->bytes.data()+start, data, end-start);
        std::fill(shadow->known.begin()+start, shadow->known.begin()+end, static_cast<unsigned char>(1)); shadow->generation = current;shadow->writeSerial=++writes;++shadow->revision;
    }
}
} // namespace taa::constants
