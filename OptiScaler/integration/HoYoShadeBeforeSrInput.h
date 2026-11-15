#pragma once
#include <inputs/BeforeSrInput.h>
#include "HoYoShadeColorContract.h"

namespace HoYoShade {
enum class BeforeSrColor { Legacy, DeclaredSdr, InvalidSource, ResourceMismatch,
    RegionMismatch, TransferUnknown, TransferUnsupported, FormatMismatch };
inline const char* Text(BeforeSrColor value) noexcept {
    switch (value) {
    case BeforeSrColor::Legacy: return "legacy-input";
    case BeforeSrColor::DeclaredSdr: return "producer-sdr";
    case BeforeSrColor::InvalidSource: return "invalid-producer-description";
    case BeforeSrColor::ResourceMismatch: return "producer-resource-mismatch";
    case BeforeSrColor::RegionMismatch: return "producer-region-mismatch";
    case BeforeSrColor::TransferUnknown: return "producer-transfer-unknown";
    case BeforeSrColor::TransferUnsupported: return "producer-transfer-unsupported";
    case BeforeSrColor::FormatMismatch: return "producer-transport-format-mismatch";
    default: return "invalid-color-admission";
    }
}
struct BeforeSrBinding {
    ID3D11DeviceContext* context = nullptr;
    ID3D11Resource* color = nullptr;
    ID3D11Resource* depth = nullptr;
    ID3D11Resource* motion = nullptr;
    DXGI_FORMAT storage = DXGI_FORMAT_UNKNOWN, transport = DXGI_FORMAT_UNKNOWN;
    UINT width = 0, height = 0, colorX = 0, colorY = 0, depthX = 0, depthY = 0;
};

// Match the declaration to the source textures retained by THIS prepared input
// read. The existing read owner, queue/fence and cache epoch retain authority.
inline bool MatchesBeforeSrInput(const interop::BeforeSrInput* source, const BeforeSrBinding& b) noexcept {
    if (!source || source->query != interop::BeforeSrQuery::Matched) return false;
    const auto& f = source->frame;
    return f.context == b.context && f.color == b.color && f.depth == b.depth && f.motion == b.motion &&
        f.render_width == b.width && f.render_height == b.height &&
        !b.colorX && !b.colorY && !b.depthX && !b.depthY;
}
inline BeforeSrColor AdmitBeforeSrInput(const interop::BeforeSrInput* source, const BeforeSrBinding& b) noexcept {
    if (!source || !source->HasDeclaration()) return BeforeSrColor::Legacy;
    if (source->query != interop::BeforeSrQuery::Matched) return BeforeSrColor::InvalidSource;
    const auto& f = source->frame;
    if (f.context != b.context || f.color != b.color || f.depth != b.depth || f.motion != b.motion)
        return BeforeSrColor::ResourceMismatch;
    if (!MatchesBeforeSrInput(source, b)) return BeforeSrColor::RegionMismatch;
    if (f.color_transfer == neo::interop::ColorTransfer::unknown) return BeforeSrColor::TransferUnknown;
    // API20's current pair of effect RTVs supports the declared SDR endpoint.
    // Linear/PQ need separate processing semantics; a typed view grants none.
    if (f.color_transfer != neo::interop::ColorTransfer::srgb) return BeforeSrColor::TransferUnsupported;
    const auto storage = ColorContract::Resolve(b.storage);
    const auto view = ColorContract::Resolve(f.color_view_format);
    if (f.color_storage_format != b.storage || b.transport != b.storage || !storage || !view ||
        storage.runtime != view.runtime) return BeforeSrColor::FormatMismatch;
    return BeforeSrColor::DeclaredSdr;
}
}
