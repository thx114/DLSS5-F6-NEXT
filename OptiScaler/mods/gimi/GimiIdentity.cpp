#include "GimiIdentity.h"
#include <array>
#include <algorithm>
#include <climits>

namespace gimi
{
namespace
{
constexpr std::array<std::uint32_t, 256> CrcTable() noexcept
{
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < table.size(); ++i)
    {
        auto crc = i;
        for (unsigned bit = 0; bit != 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0x82f63b78u : 0u);
        table[i] = crc;
    }
    return table;
}
constexpr auto Table = CrcTable();
D3D11_TEXTURE2D_DESC HashDescriptor(D3D11_TEXTURE2D_DESC desc, const HashPolicy& policy) noexcept
{
    if (policy.normalizeResolution && policy.resolutionKnown && policy.screenWidth && policy.screenHeight)
    {
        const std::uint64_t w = policy.screenWidth, h = policy.screenHeight;
        std::uint32_t replacement = 0;
        if (desc.Width == w && desc.Height == h) replacement = 0x53524553u; // 'SRES' MSVC
        else if (desc.Width == w * 2 && desc.Height == h * 2) replacement = 0x53522a32u;
        else if (desc.Width == w * 4 && desc.Height == h * 4) replacement = 0x53522a34u;
        else if (desc.Width == w * 8 && desc.Height == h * 8) replacement = 0x53522a38u;
        else if (desc.Width == w / 2 && desc.Height == h / 2) replacement = 0x53522f32u;
        if (replacement) desc.Width = desc.Height = replacement;
    }
    return desc;
}
std::uint32_t TextureDescriptorHash(std::uint32_t seed, const D3D11_TEXTURE2D_DESC& desc, const HashPolicy& p) noexcept
{
    const auto normalized = HashDescriptor(desc, p);
    static_assert(sizeof(D3D11_TEXTURE2D_DESC) == 44, "Upstream DX11 descriptor ABI");
    return ResourceCrc32c(seed, &normalized, sizeof(normalized));
}
bool SameCopyShape(const ResourceIdentity& a, const ResourceIdentity& b) noexcept
{
    if (a.kind != b.kind) return false;
    if (a.kind == IdentityKind::Buffer) return a.buffer.ByteWidth == b.buffer.ByteWidth;
    if (a.kind != IdentityKind::Texture2D) return false;
    const auto& x = a.texture; const auto& y = b.texture;
    // Strict equal formats, not just DXGI type groups: no reinterpretation proof.
    return x.Width == y.Width && x.Height == y.Height && x.MipLevels == y.MipLevels &&
        x.ArraySize == y.ArraySize && x.Format == y.Format &&
        x.SampleDesc.Count == y.SampleDesc.Count && x.SampleDesc.Quality == y.SampleDesc.Quality;
}
} // namespace
ResourceIdentity ResourceIdentity::KnownNull() noexcept
{
    ResourceIdentity result;
    result.knowledge = Knowledge::Null;
    return result;
}
std::uint32_t ResourceCrc32c(std::uint32_t seed, const void* bytes, std::size_t count) noexcept
{
    auto crc = ~seed;
    const auto* data = static_cast<const std::uint8_t*>(bytes);
    while (count--) crc = Table[(crc ^ *data++) & 0xff] ^ (crc >> 8);
    return ~crc;
}
bool HashBuffer(const D3D11_BUFFER_DESC& desc, CpuImage initial, std::uint32_t& data, std::uint32_t& hash) noexcept
{
    static_assert(sizeof(D3D11_BUFFER_DESC) == 24, "Upstream DX11 descriptor ABI");
    data = hash = 0;
    if (!desc.ByteWidth || (initial.data && initial.bytes < desc.ByteWidth)) return false;
    if (initial.data) data = ResourceCrc32c(0, initial.data, desc.ByteWidth);
    hash = ResourceCrc32c(data, &desc, sizeof(desc));
    return true;
}
bool HashTexture2D(const D3D11_TEXTURE2D_DESC& desc, CpuImage initial, const HashPolicy& policy,
                   std::uint32_t& data, std::uint32_t& hash) noexcept
{
    data = hash = 0;
    SurfaceLayout surface;
    if (policy.textureVersion > 1 || policy.trackRegionHashes ||
        (policy.normalizeResolution && (!policy.resolutionKnown || !policy.screenWidth || !policy.screenHeight)) ||
        !desc.MipLevels || desc.MipLevels > 15 || desc.ArraySize != 1 ||
        desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality ||
        !FormatSurface(desc.Format, desc.Width, desc.Height, surface)) return false;
    if (initial.data)
    {
        if (initial.rowPitch < surface.rowBytes) return false;
        const std::uint64_t reachable = std::uint64_t(initial.rowPitch) * (surface.rows - 1) + surface.rowBytes;
        // Version 0's old length uses texels, including the old UINT multiply.
        // Reject overflow rather than emulate out-of-bounds legacy arithmetic.
        const auto oldLength = std::uint64_t(desc.Width) * desc.Height * desc.ArraySize;
        if (oldLength > UINT32_MAX || surface.bytes > INT_MAX) return false;
        const bool compressed = FormatElementBytes(desc.Format) == 0;
        const std::uint64_t legacyLength = compressed ? surface.bytes : std::uint64_t(initial.rowPitch) * desc.Height;
        const bool contiguous = policy.textureVersion == 0 && oldLength <= legacyLength;
        if (contiguous)
        {
            // Creation-time zero_padding=false, exactly as the real package.
            if (initial.bytes < oldLength) return false;
            data = ResourceCrc32c(0, initial.data, static_cast<std::size_t>(oldLength));
        }
        else
        {
            if (initial.bytes < reachable) return false;
            const auto* row = static_cast<const std::uint8_t*>(initial.data);
            for (std::uint32_t y = 0; y < surface.rows; ++y)
                data = ResourceCrc32c(data, row + std::size_t(y) * initial.rowPitch, surface.rowBytes);
        }
    }
    hash = TextureDescriptorHash(data, desc, policy);
    return true;
}
bool IdentityRecord::CreateBuffer(std::uint64_t object, std::uint64_t device,
                                  const D3D11_BUFFER_DESC& desc, CpuImage initial) noexcept
{
    value_ = {}; mapped_ = 0; copiedDataPending_ = false; descriptorOnly_ = false; policy_ = {};
    if (!object || !device) return false;
    value_.knowledge = Knowledge::Known; value_.kind = IdentityKind::Buffer;
    value_.origin = ResourceOrigin::Game;
    value_.object = object; value_.deviceGeneration = device;
    value_.contentGeneration = value_.descriptorGeneration = 1;
    value_.buffer = desc;
    value_.hashKnown = HashBuffer(desc, initial, value_.dataHash, value_.upstreamHash);
    value_.creationHash = value_.upstreamHash;
    value_.creationHashKnown = value_.hashKnown;
    value_.contentKnown = value_.hashKnown && initial.data;
    return value_.hashKnown;
}
bool IdentityRecord::CreateBufferDescriptor(std::uint64_t object, std::uint64_t device,
                                            const D3D11_BUFFER_DESC& desc) noexcept
{
    value_ = {}; mapped_ = 0; copiedDataPending_ = false; descriptorOnly_ = true; policy_ = {};
    if (!object || !device || !desc.ByteWidth) return false;
    value_.knowledge = Knowledge::Known; value_.kind = IdentityKind::Buffer;
    value_.origin = ResourceOrigin::Game;
    value_.object = object; value_.deviceGeneration = device;
    value_.contentGeneration = value_.descriptorGeneration = 1;
    value_.buffer = desc;
    return true;
}
bool IdentityRecord::CreateTexture2DDescriptor(std::uint64_t object, std::uint64_t device,
                                              const D3D11_TEXTURE2D_DESC& desc) noexcept
{
    value_ = {}; mapped_ = 0; copiedDataPending_ = false; descriptorOnly_ = true; policy_ = {};
    if (!object || !device || !desc.Width || !desc.Height || !desc.MipLevels || !desc.ArraySize || !desc.SampleDesc.Count) return false;
    value_.knowledge = Knowledge::Known; value_.kind = IdentityKind::Texture2D;
    value_.origin = ResourceOrigin::Game;
    value_.object = object; value_.deviceGeneration = device;
    value_.contentGeneration = value_.descriptorGeneration = 1;
    value_.texture = desc; // actual GetDesc; no creation/content hash or read permission
    return true;
}
bool IdentityRecord::CreateTexture2D(std::uint64_t object, std::uint64_t device,
                                     const D3D11_TEXTURE2D_DESC& desc, CpuImage initial, HashPolicy policy) noexcept
{
    value_ = {}; mapped_ = 0; copiedDataPending_ = false; descriptorOnly_ = false; policy_ = policy;
    if (!object || !device) return false;
    value_.knowledge = Knowledge::Known; value_.kind = IdentityKind::Texture2D;
    value_.origin = ResourceOrigin::Game;
    value_.object = object; value_.deviceGeneration = device;
    value_.contentGeneration = value_.descriptorGeneration = 1;
    value_.texture = desc;
    value_.hashKnown = HashTexture2D(desc, initial, policy_, value_.dataHash, value_.upstreamHash);
    value_.creationHash = value_.upstreamHash;
    value_.creationHashKnown = value_.hashKnown;
    value_.contentKnown = value_.hashKnown && initial.data;
    return value_.hashKnown;
}
void IdentityRecord::Destroy() noexcept
{
    value_ = {}; mapped_ = 0; copiedDataPending_ = false; descriptorOnly_ = false;
}
void IdentityRecord::OpaqueWrite() noexcept
{
    ++value_.contentGeneration;
    value_.contentKnown = false;
    copiedDataPending_ = false;
    if (policy_.trackTextureUpdates && value_.kind == IdentityKind::Texture2D) value_.hashKnown = false;
}
void IdentityRecord::MapSucceeded(std::uint32_t subresource, D3D11_MAP mode) noexcept
{
    if (mode == D3D11_MAP_READ) return;
    mapped_ |= subresource < 31 ? (1u << subresource) : 0x80000000u;
    Update(subresource);
}
void IdentityRecord::Unmap(std::uint32_t subresource) noexcept
{
    if (subresource < 31) mapped_ &= ~(1u << subresource);
}
void IdentityRecord::Update(std::uint32_t subresource) noexcept
{
    // Subresources beyond zero don't contribute to the upstream hash; still
    // change the cache generation. Whole-resource content qualification is
    // deliberately conservative, so these also require requalification.
    if (subresource == 0) OpaqueWrite();
    else { ++value_.contentGeneration; value_.contentKnown = false; copiedDataPending_ = false; }
}
bool IdentityRecord::RefreshCpu(std::uint64_t revision, CpuImage cpu) noexcept
{
    if (descriptorOnly_ || revision != value_.contentGeneration || mapped_ || !cpu.data || value_.knowledge != Knowledge::Known) return false;
    if ((!policy_.trackTextureUpdates || value_.kind == IdentityKind::Buffer) && !value_.creationHashKnown) return false;
    std::uint32_t data = 0, hash = 0;
    bool ok = false;
    if (value_.kind == IdentityKind::Buffer) ok = HashBuffer(value_.buffer, cpu, data, hash);
    else if (value_.kind == IdentityKind::Texture2D) ok = HashTexture2D(value_.texture, cpu, policy_, data, hash);
    if (!ok) return false;
    value_.dataHash = data;
    if (policy_.trackTextureUpdates && value_.kind == IdentityKind::Texture2D) value_.upstreamHash = hash;
    value_.hashKnown = true; value_.contentKnown = true;
    copiedDataPending_ = false;
    ++value_.contentGeneration; // invalidate negative cache entries too
    return true;
}
void IdentityRecord::CopyResource(const IdentityRecord& source) noexcept
{
    // Copying data_hash is only exact with the same byte/pitch interpretation.
    // Textures carrying legacy row padding cannot be proven from just hashes;
    // require identical creation descriptors/hash policy and tight source rows
    // by limiting propagation to buffers or accurate texture hashing below.
    const auto src = source.value_;
    OpaqueWrite();
    if (descriptorOnly_ || mapped_ || source.mapped_ || src.knowledge != Knowledge::Known ||
        value_.deviceGeneration != src.deviceGeneration || !src.contentKnown ||
        !SameCopyShape(value_, src)) return;
    if (value_.kind == IdentityKind::Texture2D &&
        (policy_.textureVersion != 1 || source.policy_.textureVersion != 1)) return;
    value_.dataHash = src.dataHash; value_.contentKnown = true;
    if (policy_.trackTextureUpdates && value_.kind == IdentityKind::Texture2D)
    {
        value_.hashKnown = false;
        copiedDataPending_ = true;
    }
}
bool IdentityRecord::FinalizeCopy(std::uint64_t revision) noexcept
{
    if (revision != value_.contentGeneration || !copiedDataPending_ || mapped_ || !value_.contentKnown ||
        value_.kind != IdentityKind::Texture2D) return false;
    value_.upstreamHash = TextureDescriptorHash(value_.dataHash, value_.texture, policy_);
    value_.hashKnown = true; copiedDataPending_ = false;
    ++value_.contentGeneration;
    return true;
}
void IdentityRecord::CopySubresource(std::uint32_t dstSub, const IdentityRecord& source,
                                    std::uint32_t srcSub, std::uint32_t x, std::uint32_t y,
                                    std::uint32_t z, const D3D11_BOX* box) noexcept
{
    const auto& s = source.value_;
    bool full = !dstSub && !srcSub && !x && !y && !z && SameCopyShape(value_, s);
    if (box)
    {
        const auto w = s.kind == IdentityKind::Buffer ? s.buffer.ByteWidth : s.texture.Width;
        const auto h = s.kind == IdentityKind::Buffer ? 1u : s.texture.Height;
        full = full && !box->left && !box->top && !box->front && box->right == w && box->bottom == h && box->back == 1;
    }
    if (full && (s.kind == IdentityKind::Buffer || s.texture.MipLevels == 1)) CopyResource(source);
    else Update(dstSub);
}
} // namespace gimi
