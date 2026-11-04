#include "GimiDds.h"
#include <algorithm>
#include <cstring>

namespace gimi::detail
{
namespace
{
constexpr std::uint32_t Four(char a, char b, char c, char d) noexcept
{
    return std::uint32_t(a) | (std::uint32_t(b) << 8) | (std::uint32_t(c) << 16) | (std::uint32_t(d) << 24);
}
std::uint32_t Word(const std::vector<std::uint8_t>& b, std::size_t offset) noexcept
{
    std::uint32_t result;
    std::memcpy(&result, b.data() + offset, sizeof(result));
    return result; // Windows DX11 little-endian ABI
}
DXGI_FORMAT LegacyFormat(const std::vector<std::uint8_t>& b) noexcept
{
    const auto flags = Word(b, 80), four = Word(b, 84), bits = Word(b, 88);
    if (flags & 4)
    {
        switch (four)
        {
        case Four('D','X','T','1'): return DXGI_FORMAT_BC1_UNORM;
        case Four('D','X','T','3'): return DXGI_FORMAT_BC2_UNORM;
        case Four('D','X','T','5'): return DXGI_FORMAT_BC3_UNORM;
        case Four('A','T','I','1'): case Four('B','C','4','U'): return DXGI_FORMAT_BC4_UNORM;
        case Four('B','C','4','S'): return DXGI_FORMAT_BC4_SNORM;
        case Four('A','T','I','2'): case Four('B','C','5','U'): return DXGI_FORMAT_BC5_UNORM;
        case Four('B','C','5','S'): return DXGI_FORMAT_BC5_SNORM;
        default: return DXGI_FORMAT_UNKNOWN;
        }
    }
    const auto r = Word(b, 92), g = Word(b, 96), blue = Word(b, 100), a = Word(b, 104);
    if ((flags & 0x40) && bits == 32 && a == 0xff000000 && g == 0x0000ff00)
    {
        if (r == 0x000000ff && blue == 0x00ff0000) return DXGI_FORMAT_R8G8B8A8_UNORM;
        if (r == 0x00ff0000 && blue == 0x000000ff) return DXGI_FORMAT_B8G8R8A8_UNORM;
    }
    if ((flags & 0x20000) && bits == 8 && r == 0xff && !g && !blue && !a) return DXGI_FORMAT_R8_UNORM;
    if ((flags & 2) && bits == 8 && a == 0xff && !r && !g && !blue) return DXGI_FORMAT_A8_UNORM;
    return DXGI_FORMAT_UNKNOWN;
}
} // namespace
bool DecodeDds(const std::vector<std::uint8_t>& bytes, DdsImage& out, std::string& error)
{
    out = {};
    const auto fail = [&](const char* message) { error = message; return false; };
    if (bytes.size() < 128 || Word(bytes, 0) != Four('D','D','S',' ') || Word(bytes, 4) != 124 || Word(bytes, 76) != 32)
        return fail("Unsupported DDS header (magic/size/pixel-format size)");
    const auto flags = Word(bytes, 8), width = Word(bytes, 16), height = Word(bytes, 12);
    if ((flags & 0x1007) != 0x1007 || !width || !height || width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)
        return fail("Unsupported DDS dimensions or required flags");
    if (Word(bytes, 24) > 1 || (Word(bytes, 112) & (0xfe00u | 0x200000u)) || !(Word(bytes, 108) & 0x1000))
        return fail("Unsupported DDS cube/volume or missing texture cap");
    auto format = LegacyFormat(bytes);
    std::size_t offset = 128;
    if ((Word(bytes, 80) & 4) && Word(bytes, 84) == Four('D','X','1','0'))
    {
        if (bytes.size() < 148) return fail("Truncated DDS DX10 header");
        format = static_cast<DXGI_FORMAT>(Word(bytes, 128));
        if (Word(bytes, 132) != D3D11_RESOURCE_DIMENSION_TEXTURE2D || Word(bytes, 140) != 1 ||
            Word(bytes, 136) != 0 || Word(bytes, 144) > 4)
            return fail("Unsupported DDS DX10 dimension/array/cube/alpha metadata");
        offset = 148;
    }
    if (!SupportedTextureFormat(format)) return fail("Unsupported DDS format: typed BC1-7 or listed single-plane typed formats required");
    const auto mips = Word(bytes, 28) ? Word(bytes, 28) : 1;
    unsigned maximum = 1;
    for (auto n = (std::max)(width, height); n > 1; n >>= 1) ++maximum;
    if (mips > maximum || (mips > 1 && !(flags & 0x20000))) return fail("Invalid DDS mip count/flags");
    SurfaceLayout top;
    if (!FormatSurface(format, width, height, top)) return fail("Unsupported DDS surface layout");
    if (FormatElementBytes(format) && (flags & 8) && Word(bytes, 20) != top.rowBytes)
        return fail("Unsupported padded DDS rows; tight canonical file rows required");
    if (!FormatElementBytes(format) && (flags & 0x80000) && Word(bytes, 20) != top.bytes)
        return fail("Unsupported DDS compressed linear size");
    out.descriptor = {width, height, mips, 1, format, {1, 0}, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0};
    for (unsigned mip = 0; mip < mips; ++mip)
    {
        SurfaceLayout surface;
        if (!FormatSurface(format, (std::max)(1u, width >> mip), (std::max)(1u, height >> mip), surface) ||
            surface.bytes > UINT32_MAX || surface.bytes > bytes.size() - offset)
            return fail("Truncated DDS mip payload or size overflow");
        out.mips.push_back({offset, surface.rowBytes, static_cast<std::uint32_t>(surface.bytes)});
        offset += static_cast<std::size_t>(surface.bytes);
    }
    if (offset != bytes.size()) return fail("Unsupported DDS trailing payload/array data");
    return true;
}
} // namespace gimi::detail
