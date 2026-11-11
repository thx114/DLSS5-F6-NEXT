#pragma once
#include <dxgiformat.h>
#include "hoyoshade/sdk/reshade/reshade_api_format.hpp"

namespace HoYoShade::ColorContract
{
// A storage/view tuple, not transfer, gamut, whitepoint, exposure or alpha
// authority. AdmitColor still authenticates the endpoint's independent contract.
// The existing API20 path copies all four channels without adapter conversion;
// the selected techniques retain their original sRGB read/write states.
struct Formats
{
    DXGI_FORMAT endpoint = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT runtime = DXGI_FORMAT_UNKNOWN;
    reshade::api::format working = reshade::api::format::unknown;
    reshade::api::format linear = reshade::api::format::unknown;
    reshade::api::format srgb = reshade::api::format::unknown;
    constexpr explicit operator bool() const noexcept { return runtime != DXGI_FORMAT_UNKNOWN; }
};

// Keep endpoint, private swapchain, working resource and both effect RTVs in
// one domain. Typeless endpoints have no typed-view receipt here. FP16/PQ/HLG
// need an authenticated endpoint AND core external-target color semantics;
// recognizing a format enum or allocating a float texture supplies neither.
constexpr Formats Resolve(DXGI_FORMAT endpoint) noexcept
{
    using F = reshade::api::format;
    switch (endpoint) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return {endpoint, DXGI_FORMAT_R8G8B8A8_UNORM, F::r8g8b8a8_typeless,
            F::r8g8b8a8_unorm, F::r8g8b8a8_unorm_srgb};
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return {endpoint, DXGI_FORMAT_B8G8R8A8_UNORM, F::b8g8r8a8_typeless,
            F::b8g8r8a8_unorm, F::b8g8r8a8_unorm_srgb};
    default:
        return {};
    }
}

// Private DXGI swapchains use the base typed format, never typeless/sRGB.
constexpr bool IsRuntimeFormat(DXGI_FORMAT format) noexcept
{
    const auto formats = Resolve(format);
    return formats && formats.runtime == format;
}
}
