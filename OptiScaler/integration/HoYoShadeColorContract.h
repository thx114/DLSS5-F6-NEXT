#pragma once
#include <cstdint>
#include <dxgiformat.h>
#include "hoyoshade/sdk/reshade/reshade_api_format.hpp"

namespace HoYoShade::ColorContract
{
// A storage/view tuple, not transfer, gamut, whitepoint, exposure or alpha
// authority. AdmitColor still authenticates the endpoint's independent contract.
// The existing API20 path copies all four channels without adapter conversion;
// the selected techniques retain their original sRGB read/write states.
enum class Domain { Srgb, SourceValues };
struct Formats
{
    DXGI_FORMAT endpoint = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT runtime = DXGI_FORMAT_UNKNOWN;
    reshade::api::format working = reshade::api::format::unknown;
    reshade::api::format linear = reshade::api::format::unknown;
    reshade::api::format srgb = reshade::api::format::unknown;
    Domain domain = Domain::Srgb;
    constexpr explicit operator bool() const noexcept { return runtime != DXGI_FORMAT_UNKNOWN; }
};

// Display-encoded SDR path. SourceValues below is a separate numeric execution
// contract; a float format never authenticates scene-linear or display transfer.
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

// The private swapchain bootstraps the runtime only. API20 selects the actual
// effect permutation from the external target. Keep R11/FP16/F32 in their own
// format and feed identical typed views to both slots: no hidden sRGB conversion,
// tone mapping, exposure scaling or intermediate 8bit quantization.
constexpr Formats ResolveSource(DXGI_FORMAT endpoint) noexcept
{
    using F = reshade::api::format;
    auto sdr=Resolve(endpoint);
    if(sdr) { sdr.srgb=sdr.linear; sdr.domain=Domain::SourceValues; return sdr; }
    switch(endpoint) {
    case DXGI_FORMAT_R11G11B10_FLOAT:
        return {endpoint, DXGI_FORMAT_R16G16B16A16_FLOAT, F::r11g11b10_float,
            F::r11g11b10_float, F::r11g11b10_float, Domain::SourceValues};
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return {endpoint, endpoint, F::r16g16b16a16_float,
            F::r16g16b16a16_float, F::r16g16b16a16_float, Domain::SourceValues};
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return {endpoint, DXGI_FORMAT_R16G16B16A16_FLOAT, F::r32g32b32a32_float,
            F::r32g32b32a32_float, F::r32g32b32a32_float, Domain::SourceValues};
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return {endpoint, DXGI_FORMAT_R16G16B16A16_FLOAT, F::r10g10b10a2_unorm,
            F::r10g10b10a2_unorm, F::r10g10b10a2_unorm, Domain::SourceValues};
    default: return {};
    }
}

// The producer supplies the source storage and its actual typed view. The
// retained bridge input supplies the transport allocation, which may remain
// typeless. This tuple describes byte-compatible copies, never color transfer.
struct InputFormats
{
    DXGI_FORMAT storage = DXGI_FORMAT_UNKNOWN, view = DXGI_FORMAT_UNKNOWN;
    Formats transport;
    constexpr explicit operator bool() const noexcept { return bool(transport); }
};
constexpr InputFormats ResolveInput(DXGI_FORMAT storage, DXGI_FORMAT view, DXGI_FORMAT transport,
                                    Domain domain = Domain::Srgb) noexcept
{
    const auto typed = domain == Domain::SourceValues ? ResolveSource(view) : Resolve(view);
    if (!typed) return {};
    if (storage == DXGI_FORMAT_R8G8B8A8_TYPELESS) {
        if (typed.runtime != DXGI_FORMAT_R8G8B8A8_UNORM ||
            (transport != DXGI_FORMAT_R8G8B8A8_TYPELESS &&
             transport != DXGI_FORMAT_R8G8B8A8_UNORM && transport != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)) return {};
        auto formats = typed;
        formats.endpoint = transport; // Preserve the actual allocation, not the producer's view.
        return {storage, view, formats};
    }
    const auto formats = domain == Domain::SourceValues ? ResolveSource(storage) : Resolve(storage);
    if (!formats || formats.working != typed.working || transport != storage) return {};
    return {storage, view, formats};
}

// Same-call value, bound after source identity and explicit SDR admission.
// It owns no resource and cannot authorize readiness, submission or retirement.
// The existing input owner keeps the actual API objects alive.
struct DeclaredInput
{
    InputFormats formats;
    const void* color = nullptr;
    const void* queue = nullptr;
    uint64_t stream = 0, generation = 0, frame = 0;
    uint32_t x = 0, y = 0, width = 0, height = 0;
    constexpr explicit operator bool() const noexcept { return bool(formats); }
    template<class Endpoint>
    static DeclaredInput Bind(const InputFormats& formats, const Endpoint& e) noexcept
    {
        if (!formats || (!e.sdr && formats.transport.domain != Domain::SourceValues)) return {};
        return {formats, e.color, e.queue, e.stream, e.generation, e.frame,
            e.colorX, e.colorY, e.width, e.height};
    }
    template<class Endpoint>
    bool Matches(const Endpoint& e, DXGI_FORMAT actual) const noexcept
    {
        return formats && color && queue && stream && generation && frame && width && height &&
            formats.transport.endpoint == actual && color == e.color && queue == e.queue &&
            stream == e.stream && generation == e.generation && frame == e.frame &&
            x == e.colorX && y == e.colorY && width == e.width && height == e.height;
    }
};

// Private DXGI swapchains use the base typed format, never typeless/sRGB.
constexpr bool IsRuntimeFormat(DXGI_FORMAT format) noexcept
{
    const auto formats = Resolve(format);
    return (formats && formats.runtime == format) || format == DXGI_FORMAT_R16G16B16A16_FLOAT;
}
}
