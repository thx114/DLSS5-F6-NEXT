#pragma once
#include "HoYoShadeApi.h"
#include <native/FrameContract.h>
#include <wrl/client.h>
#include <memory>
#include <cstdint>

namespace GenshinTransport { class Lease; }
namespace GenshinGuides { struct Owner; }
namespace HoYoShade::Depth
{
struct Access;
// The caller supplies a genuine, content-owning reader. Keeping a native COM
// texture alive is insufficient: its producer must not recycle its CONTENTS.
// Validate includes exact producer epoch, resource, queue and ready ordering.
// BindCompletion is called only with a fence queued AFTER the actual reader.
// Missing completion, device removal and ambiguous recording quarantine it.
class Reader
{
public:
    virtual ~Reader() = default; // CPU ownership only, no SDK destruction
    virtual bool Validate(const Access&) const noexcept = 0;
    virtual bool BindCompletion(ID3D12Fence*, uint64_t) noexcept = 0;
    virtual void Quarantine() noexcept = 0;
};
namespace Detail
{
// Small CPU state machine shared by every SDK adapter. It does not mint GPU
// evidence: only the caller's Reader can accept an actual completion fence.
class ReadLifetime final
{
    std::shared_ptr<Reader> reader_;
    bool began_ = false, restored_ = false, completed_ = false, quarantined_ = false;
public:
    explicit ReadLifetime(std::shared_ptr<Reader> reader) : reader_(std::move(reader)) {}
    ReadLifetime(const ReadLifetime&) = delete;
    ReadLifetime& operator=(const ReadLifetime&) = delete;
    ~ReadLifetime() { if (began_ && !completed_) Quarantine(); }
    bool Began() const noexcept { return began_; }
    bool Completed() const noexcept { return completed_; }
    bool Quarantined() const noexcept { return quarantined_; }
    bool Begin() noexcept { if (began_ || !reader_) return false; began_ = true; return true; }
    void Restored() noexcept { restored_ = began_; }
    void Quarantine() noexcept {
        if (!quarantined_ && reader_) { quarantined_ = true; reader_->Quarantine(); }
    }
    bool Complete(ID3D12Fence* fence, uint64_t value) noexcept {
        if (!began_) return true;
        if (!restored_ || completed_ || !reader_->BindCompletion(fence, value)) { Quarantine(); return false; }
        completed_ = true; return true;
    }
};
}
struct Access
{
    ID3D12Resource* resource = nullptr;
    DXGI_FORMAT typedFormat = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0, height = 0, subresource = 0;
    D3D12_RESOURCE_STATES entryState = D3D12_RESOURCE_STATE_COMMON;
    bool stateKnown = false, directionKnown = false, reversed = false, yFlipped = false;
    bool validityPaired = false;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12Fence* readyFence = nullptr;
    uint64_t readyValue = 0, owner = 0, generation = 0, frame = 0, epoch = 0;
    std::shared_ptr<Reader> reader;
};
using Input = Access;
inline constexpr D3D12_RESOURCE_STATES ShaderRead = static_cast<D3D12_RESOURCE_STATES>(
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
// Deliberately narrow: exact 2D, one plane/mip/array/sample. No arbitrary
// subresource reinterpretation, MSAA guess, stencil, or numerical Z conversion.
inline bool TypedFormatCompatible(DXGI_FORMAT resource, DXGI_FORMAT view) noexcept
{
    switch (view) {
    case DXGI_FORMAT_R32_FLOAT: return resource == view || resource == DXGI_FORMAT_R32_TYPELESS;
    case DXGI_FORMAT_R16_UNORM: return resource == view || resource == DXGI_FORMAT_R16_TYPELESS;
    case DXGI_FORMAT_R16_FLOAT: return resource == view || resource == DXGI_FORMAT_R16_TYPELESS;
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: return resource == DXGI_FORMAT_R24G8_TYPELESS;
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return resource == DXGI_FORMAT_R32G8X24_TYPELESS;
    case DXGI_FORMAT_R32G32_FLOAT: return resource == view || resource == DXGI_FORMAT_R32G32_TYPELESS;
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return resource == view || resource == DXGI_FORMAT_R32G32B32A32_TYPELESS;
    default: return false;
    }
}
inline bool SupportedState(D3D12_RESOURCE_STATES state) noexcept
{
    return state == D3D12_RESOURCE_STATE_COMMON || state == ShaderRead ||
        state == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE || state == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
}
inline bool Compatible(const D3D12_RESOURCE_DESC& d, const Access& a) noexcept
{
    return a.width && a.height && a.subresource == 0 && a.stateKnown && SupportedState(a.entryState) &&
        a.directionKnown && d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        d.Width == a.width && d.Height == a.height && d.DepthOrArraySize == 1 && d.MipLevels == 1 &&
        d.SampleDesc.Count == 1 && !d.SampleDesc.Quality && !(d.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) &&
        TypedFormatCompatible(d.Format, a.typedFormat) && (!a.validityPaired ||
            a.typedFormat == DXGI_FORMAT_R32G32_FLOAT || a.typedFormat == DXGI_FORMAT_R32G32B32A32_FLOAT);
}

// No registry, core, loader or global ownership. The caller must clear DEPTH
// and prove its preceding GPU batch complete BEFORE replacing/destroying views.
// Owned callers embed Cache in an adopted Resource; natural callers put it in
// their existing domain owner. Destroy is explicit while the SDK device lives.
struct Cache
{
    Cache() = default;
    Cache(const Cache&) = delete;
    Cache& operator=(const Cache&) = delete;
    reshade::api::device* device = nullptr; // borrowed exact SDK owner of srv
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    reshade::api::resource_view srv{0};
    DXGI_FORMAT typedFormat = DXGI_FORMAT_UNKNOWN;
    bool quarantined = false; // sticky for this caller-owned cache lifetime
    void Destroy(reshade::api::device*) noexcept;
};
class Prepared final
{
    Access input_;
    reshade::api::device* device_ = nullptr; // borrowed synchronous SDK interval
    reshade::api::effect_runtime* runtime_ = nullptr;
    reshade::api::command_list* list_ = nullptr;
    ID3D12GraphicsCommandList* nativeList_ = nullptr;
    reshade::api::resource_view srv_{0};
    Cache* cache_ = nullptr; // caller retains Cache through Prepared and its GPU batch
    Detail::ReadLifetime lifetime_;
    bool restored_ = false;
    void Transition(ID3D12GraphicsCommandList*, bool restore);
public:
    Prepared(const Access&, reshade::api::device*, reshade::api::resource_view, Cache&);
    ~Prepared();
    Prepared(const Prepared&) = delete;
    Prepared& operator=(const Prepared&) = delete;
    // The list must be the real SDK list associated with nativeList; neither
    // function submits a list or signals a fence. No owned-lease dependency.
    bool Begin(reshade::api::effect_runtime*, reshade::api::command_list*, ID3D12GraphicsCommandList*);
    void End(reshade::api::effect_runtime*, reshade::api::command_list*, ID3D12GraphicsCommandList*);
    bool Complete(ID3D12Fence*, uint64_t) noexcept;
};
// May throw for an SDK exception. Resource/reader admission refusal returns
// null, so ordinary color effects continue with a cleared DEPTH semantic.
std::shared_ptr<Prepared> Prepare(const Access&, ID3D12Device*, reshade::api::device*, Cache&);
void Clear(reshade::api::effect_runtime*); // propagates SDK errors; never renders stale depth

// Owned-stage adapters. A missing real reader leaves the optional input empty.
Input FromNative(const native::FrameInput&, ID3D12CommandQueue*, const native::SyncPoint& producerReady,
                 uint64_t stream, uint64_t generation) noexcept;
Input FromBeforeSr(const std::shared_ptr<GenshinTransport::Lease>&, const GenshinGuides::Owner&,
    ID3D12CommandQueue*, ID3D12Fence*, uint64_t readyValue, uint64_t stream, uint64_t generation,
    uint64_t frame, uint32_t width, uint32_t height, bool directionKnown, bool reversed) noexcept;
}
