#pragma once
#include <d3d11.h>
#include <cstdint>

namespace interop {
// F6-owned, synchronous input facts. Adapters describe the current SR request;
// neither this value nor its counters grant GPU readiness or retirement.
enum class BeforeSrQuery { NotRequested, NoExport, NoMatch, Failed, Invalid, Matched };
inline const char* Text(BeforeSrQuery value) noexcept {
    switch (value) {
    case BeforeSrQuery::NotRequested: return "not-requested";
    case BeforeSrQuery::NoExport: return "no-export";
    case BeforeSrQuery::NoMatch: return "no-current-match";
    case BeforeSrQuery::Failed: return "query-failed";
    case BeforeSrQuery::Invalid: return "invalid-description";
    case BeforeSrQuery::Matched: return "current-input";
    default: return "invalid-query-state";
    }
}
enum class SrInputOrigin { Ngx, NeoBridge, Taa };
enum class ColorTransfer : uint32_t { unknown, srgb, linear, pq };
// SourceValues preserves stored numeric values. It does not claim a display
// transfer, primaries, whitepoint or exposure and never inserts a color transform.
enum class EffectDomain { TransferQualified, SourceValues };
inline constexpr uint32_t k_sr_depth_inverted = 1u << 0;
inline constexpr uint32_t k_sr_motion_jittered = 1u << 1;
inline constexpr uint32_t k_sr_reset = 1u << 2;
struct SrInputFrameDx11 {
    uint64_t frame = 0, instance = 0, generation = 0;
    ID3D11DeviceContext* context = nullptr;
    ID3D11Texture2D *color = nullptr, *depth = nullptr, *motion = nullptr;
    uint32_t render_width = 0, render_height = 0, display_width = 0, display_height = 0;
    DXGI_FORMAT color_storage_format = DXGI_FORMAT_UNKNOWN, color_view_format = DXGI_FORMAT_UNKNOWN;
    ColorTransfer color_transfer = ColorTransfer::unknown;
    EffectDomain effect_domain = EffectDomain::TransferQualified;
    // A TAA profile supplied an SR HDR flag. This does not declare a transfer.
    bool sr_hdr_declared = false;
    uint32_t flags = 0;
    bool guides_known = false;
    float jitter_x = 0, jitter_y = 0, motion_scale_x = 1, motion_scale_y = 1;
    // Frozen input and original game SRV are distinct objects. This records the
    // actual copy provenance without claiming that typeless storage became typed.
    ID3D11Texture2D* source_color = nullptr;
    DXGI_FORMAT source_storage_format = DXGI_FORMAT_UNKNOWN, source_view_format = DXGI_FORMAT_UNKNOWN;
};
struct BeforeSrInput {
    BeforeSrQuery query = BeforeSrQuery::NotRequested;
    SrInputOrigin origin = SrInputOrigin::Ngx;
    SrInputFrameDx11 frame;
    const void* parameters = nullptr;
    uint32_t feature = 0;
    uint64_t request = 0;
    bool HasDeclaration() const noexcept {
        return query == BeforeSrQuery::Failed || query == BeforeSrQuery::Invalid || query == BeforeSrQuery::Matched;
    }
    bool MatchesCall(ID3D11DeviceContext* context, const void* p, uint32_t id, uint64_t serial) const noexcept {
        return query == BeforeSrQuery::Matched && frame.context == context && parameters == p &&
            feature == id && request == serial;
    }
    bool TaaHdrFlagDeclared(ID3D11DeviceContext* context, const void* p, uint32_t id,
                            ID3D11Resource* color, ID3D11Resource* depth, ID3D11Resource* motion,
                            bool userOverride) const noexcept {
        return origin == SrInputOrigin::Taa && request && MatchesCall(context,p,id,request) &&
            frame.color == color && frame.depth == depth && frame.motion == motion &&
            (frame.sr_hdr_declared || userOverride);
    }
};
inline bool BeforeSrTextureMatches(ID3D11Texture2D* texture, UINT width, UINT height,
                                  DXGI_FORMAT format) noexcept {
    if (!texture) return false;
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    return desc.Width == width && desc.Height == height && desc.Format == format &&
        desc.MipLevels == 1 && desc.ArraySize == 1 && desc.SampleDesc.Count == 1;
}
} // namespace interop
