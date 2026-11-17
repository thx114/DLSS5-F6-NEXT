#pragma once
#include "BeforeSrInput.h"
#include "NeoBeforeSrV1.h"

namespace interop {
// The external bridge ABI is confined to this ingress adapter. TAA and ordinary
// NGX inputs do not load/query a bridge and never fabricate a Neo frame.
inline BeforeSrInput QueryBeforeSrInput(const void* caller, ID3D11DeviceContext* context,
    ID3D11Resource* color, ID3D11Resource* depth, ID3D11Resource* motion) noexcept {
    BeforeSrInput result;
    result.origin = SrInputOrigin::NeoBridge;
    result.query = BeforeSrQuery::Invalid;
    if (!caller || !context || !color || !depth || !motion) return result;
    result.query = BeforeSrQuery::NoExport;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(caller), &module)) return result;
    const auto query = reinterpret_cast<neo::interop::QueryBeforeSrFrameV1>(
        GetProcAddress(module, neo::interop::k_query_before_sr_frame_v1));
    if (query) {
        neo::interop::BeforeSrFrameV1 f;
        const auto hr = query(context, color, depth, &f);
        result.query = hr == S_FALSE ? BeforeSrQuery::NoMatch :
            hr == S_OK ? BeforeSrQuery::Invalid : BeforeSrQuery::Failed;
        if (hr == S_OK && f.size == sizeof(f) && f.version == 1 && f.context == context &&
            f.color == color && f.depth == depth && f.motion == motion &&
            f.render_width && f.render_height && f.display_width && f.display_height &&
            context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE &&
            BeforeSrTextureMatches(f.color, f.render_width, f.render_height, f.color_storage_format) &&
            BeforeSrTextureMatches(f.depth, f.render_width, f.render_height, DXGI_FORMAT_R32_FLOAT) &&
            BeforeSrTextureMatches(f.motion, f.render_width, f.render_height, DXGI_FORMAT_R16G16_FLOAT)) {
            auto& out = result.frame;
            out.frame=f.frame; out.instance=f.instance; out.context=f.context;
            out.color=f.color; out.depth=f.depth; out.motion=f.motion;
            out.render_width=f.render_width; out.render_height=f.render_height;
            out.display_width=f.display_width; out.display_height=f.display_height;
            out.color_storage_format=f.color_storage_format; out.color_view_format=f.color_view_format;
            out.color_transfer=static_cast<ColorTransfer>(f.color_transfer);
            out.flags=f.flags; out.guides_known=true;
            out.jitter_x=f.jitter_x; out.jitter_y=f.jitter_y;
            out.motion_scale_x=f.motion_scale_x; out.motion_scale_y=f.motion_scale_y;
            out.source_color=f.color; out.source_storage_format=f.color_storage_format;
            out.source_view_format=f.color_view_format;
            result.query=BeforeSrQuery::Matched;
        }
    }
    FreeLibrary(module);
    return result;
}
} // namespace interop
