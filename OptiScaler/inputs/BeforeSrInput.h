#pragma once
#include "NeoBeforeSrV1.h"

namespace interop {
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

// Stack-owned description of this synchronous V1 call. No recent-frame cache,
// registration or ownership/completion authority. The caller owns all resources.
struct BeforeSrInput {
    BeforeSrQuery query = BeforeSrQuery::NotRequested;
    neo::interop::BeforeSrFrameV1 frame;
    bool HasDeclaration() const noexcept {
        return query == BeforeSrQuery::Failed || query == BeforeSrQuery::Invalid || query == BeforeSrQuery::Matched;
    }
};

inline bool BeforeSrTextureMatches(ID3D11Texture2D* texture, UINT width, UINT height,
                                  DXGI_FORMAT format) noexcept {
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    return desc.Width == width && desc.Height == height && desc.Format == format &&
        desc.MipLevels == 1 && desc.ArraySize == 1 && desc.SampleDesc.Count == 1;
}

inline BeforeSrInput QueryBeforeSrInput(const void* caller, ID3D11DeviceContext* context,
    ID3D11Resource* color, ID3D11Resource* depth, ID3D11Resource* motion) noexcept {
    BeforeSrInput result;
    result.query = BeforeSrQuery::Invalid;
    if (!caller || !context || !color || !depth || !motion) return result;
    result.query = BeforeSrQuery::NoExport;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(caller), &module)) return result;
    // Resolve against the active caller, including renamed bridges. Hold just
    // this query's module reference; never pin, load or enumerate other DLLs.
    const auto query = reinterpret_cast<neo::interop::QueryBeforeSrFrameV1>(
        GetProcAddress(module, neo::interop::k_query_before_sr_frame_v1));
    if (query) {
        const auto hr = query(context, color, depth, &result.frame);
        result.query = hr == S_FALSE ? BeforeSrQuery::NoMatch :
            hr == S_OK ? BeforeSrQuery::Invalid : BeforeSrQuery::Failed;
        if (hr == S_OK) {
            const auto& f = result.frame;
            // Check raw ABI identities before dereferencing returned textures.
            if (f.size == sizeof(f) && f.version == 1 && f.context == context &&
                f.color == color && f.depth == depth && f.motion == motion &&
                f.render_width && f.render_height && f.display_width && f.display_height &&
                context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE &&
                BeforeSrTextureMatches(f.color, f.render_width, f.render_height, f.color_storage_format) &&
                BeforeSrTextureMatches(f.depth, f.render_width, f.render_height, DXGI_FORMAT_R32_FLOAT) &&
                BeforeSrTextureMatches(f.motion, f.render_width, f.render_height, DXGI_FORMAT_R16G16_FLOAT))
                result.query = BeforeSrQuery::Matched;
        }
    }
    FreeLibrary(module);
    return result;
}
}
