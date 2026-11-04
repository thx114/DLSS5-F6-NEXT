#pragma once

#include <d3d11.h>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace gimi
{
// Deliberately typed, single-plane formats only. Shared by identity and DDS;
// unknown/typeless/planar formats never acquire an invented byte size.
struct SurfaceLayout
{
    std::uint32_t rowBytes = 0, rows = 0;
    std::uint64_t bytes = 0;
};
std::uint32_t FormatElementBytes(DXGI_FORMAT format) noexcept;
bool FormatSurface(DXGI_FORMAT format, std::uint32_t width, std::uint32_t height,
                   SurfaceLayout& out) noexcept;
DXGI_FORMAT ParseResourceFormat(std::string_view name);
bool SupportedTextureFormat(DXGI_FORMAT format) noexcept;
} // namespace gimi
