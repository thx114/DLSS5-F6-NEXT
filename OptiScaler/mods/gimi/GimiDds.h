#pragma once

#include "GimiFormats.h"
#include <vector>
#include <string>

namespace gimi::detail
{
struct ImageMip
{
    std::size_t offset = 0;
    std::uint32_t rowPitch = 0, slicePitch = 0;
};
struct DdsImage
{
    D3D11_TEXTURE2D_DESC descriptor{};
    std::vector<ImageMip> mips;
};
// Parse a bounded immutable byte vector; native BC blocks stay compressed for
// DX11. No WIC/DirectXTK dependency or implicit format conversion/decompression.
bool DecodeDds(const std::vector<std::uint8_t>& bytes, DdsImage& out, std::string& error);
} // namespace gimi::detail
