#pragma once

// Background-only lossless raw and PNG writer. No WIC/DXC/new link dependencies.
// PNG rows are streamed as stored DEFLATE blocks: bounded memory, lossless pixels.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace DlssNrCurrentOutput
{
enum class Pixels { Rgba8, Bgra8, Rgb10A2, Half4, Float4 };
struct ImageInfo
{
    Pixels pixels = Pixels::Rgba8;
    unsigned width = 0, height = 0, bytesPerPixel = 0;
    // 1 linear HDR, 2 sRGB, 3 gamma 2.2, 4 PQ. Zero means unknown.
    unsigned encoding = 0;
};

inline unsigned PixelBytes(Pixels pixels)
{
    switch (pixels)
    {
    case Pixels::Rgba8: case Pixels::Bgra8: case Pixels::Rgb10A2: return 4;
    case Pixels::Half4: return 8;
    case Pixels::Float4: return 16;
    default: return 0;
    }
}

inline float HalfToFloat(uint16_t half)
{
    const unsigned sign = half >> 15, exponent = (half >> 10) & 31, mantissa = half & 1023;
    float value = exponent == 0 ? std::ldexp(float(mantissa), -24)
                : exponent == 31 ? (mantissa ? std::numeric_limits<float>::quiet_NaN()
                                             : std::numeric_limits<float>::infinity())
                                 : std::ldexp(1.0f + float(mantissa) / 1024.0f, int(exponent) - 15);
    return sign ? -value : value;
}

inline std::array<float, 4> ReadPixel(const uint8_t* p, Pixels kind)
{
    std::array<float, 4> c {};
    switch (kind)
    {
    case Pixels::Rgba8: case Pixels::Bgra8:
        for (unsigned i = 0; i < 4; ++i) c[i] = p[i] / 255.0f;
        if (kind == Pixels::Bgra8) std::swap(c[0], c[2]);
        break;
    case Pixels::Rgb10A2:
    {
        uint32_t bits = 0; std::memcpy(&bits, p, sizeof(bits));
        for (unsigned i = 0; i < 3; ++i) c[i] = float((bits >> (i * 10)) & 1023) / 1023.0f;
        c[3] = float(bits >> 30) / 3.0f;
        break;
    }
    case Pixels::Half4:
        for (unsigned i = 0; i < 4; ++i)
        { uint16_t bits = 0; std::memcpy(&bits, p + 2 * i, 2); c[i] = HalfToFloat(bits); }
        break;
    case Pixels::Float4: std::memcpy(c.data(), p, sizeof(float) * 4); break;
    }
    return c;
}

inline float PreviewChannel(float x, unsigned encoding)
{
    if (!std::isfinite(x)) return 0.0f;
    x = (std::max)(0.0f, x);
    if (encoding == 4)
    {
        // ST2084 EOTF to nits, 203-nit reference white, then a preview-only Reinhard curve.
        const double p = std::pow((std::min)(double(x), 1.0), 1.0 / (2523.0 / 32.0));
        const double denominator = 2413.0 / 128.0 - 2392.0 / 128.0 * p;
        x = denominator > 0.0 ? float(10000.0 / 203.0 * std::pow(
            (std::max)(p - 3424.0 / 4096.0, 0.0) / denominator, 1.0 / (2610.0 / 16384.0))) : 0.0f;
    }
    if (encoding == 1 || encoding == 4)
    {
        x = x / (1.0f + x);
        x = x <= 0.0031308f ? x * 12.92f : 1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f;
    }
    return std::clamp(x, 0.0f, 1.0f);
}

inline void WriteBe(std::ostream& out, uint32_t x)
{
    const char b[] { char(x >> 24), char(x >> 16), char(x >> 8), char(x) };
    out.write(b, 4);
}
inline uint32_t Crc(const uint8_t* p, size_t size, uint32_t crc = 0xffffffffu)
{
    for (size_t i = 0; i < size; ++i)
    {
        crc ^= p[i];
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return crc;
}
inline void Chunk(std::ostream& out, const char* type, const uint8_t* p, size_t size)
{
    WriteBe(out, static_cast<uint32_t>(size)); out.write(type, 4);
    if (size) out.write(reinterpret_cast<const char*>(p), static_cast<std::streamsize>(size));
    WriteBe(out, Crc(p, size, Crc(reinterpret_cast<const uint8_t*>(type), 4)) ^ 0xffffffffu);
}

inline void WritePng(const std::filesystem::path& path, const ImageInfo& info,
                     const std::vector<uint8_t>& tight)
{
    const auto bytes = PixelBytes(info.pixels);
    if (!bytes || info.bytesPerPixel != bytes || !info.width || !info.height ||
        uint64_t(info.width) * info.height > (std::numeric_limits<size_t>::max)() / bytes ||
        tight.size() != uint64_t(info.width) * info.height * bytes)
        throw std::runtime_error("Invalid capture image layout");
    std::ofstream out(path, std::ios::binary);
    out.exceptions(std::ios::badbit | std::ios::failbit);
    const uint8_t signature[] { 137, 80, 78, 71, 13, 10, 26, 10 };
    out.write(reinterpret_cast<const char*>(signature), sizeof(signature));
    std::array<uint8_t, 13> ihdr {};
    for (unsigned i = 0; i < 4; ++i)
    { ihdr[i] = uint8_t(info.width >> (24 - i * 8)); ihdr[i + 4] = uint8_t(info.height >> (24 - i * 8)); }
    ihdr[8] = 16; ihdr[9] = 6; // RGBA16 derivative. Original quantized/float bytes are in output.raw.
    Chunk(out, "IHDR", ihdr.data(), ihdr.size());
    if (info.encoding == 2)
    { const uint8_t intent = 0; Chunk(out, "sRGB", &intent, 1); }
    else if (info.encoding == 3)
    { const uint8_t gamma[] { 0, 0, 177, 143 }; Chunk(out, "gAMA", gamma, 4); } // 45455
    // Linear/PQ primaries are not established by their transfer function. Their
    // derivative is deliberately untagged; raw + metadata remain authoritative.
    const uint8_t zlib[] { 0x78, 0x01 }; Chunk(out, "IDAT", zlib, 2);
    uint32_t adlerA = 1, adlerB = 0;
    std::vector<uint8_t> block; block.reserve(65540);
    const auto emit = [&](bool final) {
        const uint16_t size = static_cast<uint16_t>(block.size());
        std::vector<uint8_t> payload; payload.reserve(block.size() + 5);
        payload.push_back(final ? 1 : 0);
        payload.push_back(uint8_t(size)); payload.push_back(uint8_t(size >> 8));
        payload.push_back(uint8_t(~size)); payload.push_back(uint8_t((~size) >> 8));
        payload.insert(payload.end(), block.begin(), block.end());
        Chunk(out, "IDAT", payload.data(), payload.size()); block.clear();
    };
    const auto append = [&](uint8_t b) {
        block.push_back(b); adlerA = (adlerA + b) % 65521u; adlerB = (adlerB + adlerA) % 65521u;
        if (block.size() == 65535) emit(false);
    };
    for (unsigned y = 0; y < info.height; ++y)
    {
        append(0); // PNG filter None
        for (unsigned x = 0; x < info.width; ++x)
        {
            auto c = ReadPixel(tight.data() + (uint64_t(y) * info.width + x) * info.bytesPerPixel, info.pixels);
            for (unsigned i = 0; i < 4; ++i)
            {
                const float value = i == 3 ? (std::isfinite(c[i]) ? std::clamp(c[i], 0.0f, 1.0f) : 0.0f)
                                          : PreviewChannel(c[i], info.encoding);
                const uint16_t code = static_cast<uint16_t>(std::lround(value * 65535.0f));
                append(uint8_t(code >> 8)); append(uint8_t(code));
            }
        }
    }
    emit(true);
    const uint32_t adler = (adlerB << 16) | adlerA;
    const uint8_t sum[] { uint8_t(adler >> 24), uint8_t(adler >> 16), uint8_t(adler >> 8), uint8_t(adler) };
    Chunk(out, "IDAT", sum, 4); Chunk(out, "IEND", nullptr, 0);
    out.close();
}
} // namespace DlssNrCurrentOutput
