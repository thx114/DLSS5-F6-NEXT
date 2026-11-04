#pragma once

#include "GimiFormats.h"

namespace gimi
{
enum class Knowledge : std::uint8_t { Unknown, Null, Known };
enum class IdentityKind : std::uint8_t { Unknown, Buffer, Texture2D };
enum class ResourceOrigin : std::uint8_t { Unknown, Game, Custom };
struct CpuImage
{
    const void* data = nullptr;
    std::size_t bytes = 0; // accessible bytes, never inferred from a GPU pointer
    std::uint32_t rowPitch = 0;
};
struct HashPolicy
{
    std::uint32_t textureVersion = 0; // only upstream 0/1 supported
    bool trackTextureUpdates = false; // GIMI d3dx.ini uses 0
    bool trackRegionHashes = false; // optional XXMI mode; unsupported in this slice
    bool normalizeResolution = true; // GIMI get_resolution_from=swap_chain
    bool resolutionKnown = false;
    std::uint32_t screenWidth = 0, screenHeight = 0;
};
struct ResourceIdentity
{
    Knowledge knowledge = Knowledge::Unknown;
    IdentityKind kind = IdentityKind::Unknown;
    std::uint64_t object = 0, deviceGeneration = 0, contentGeneration = 0;
    std::uint64_t descriptorGeneration = 0;
    std::uint32_t upstreamHash = 0, creationHash = 0, dataHash = 0;
    bool hashKnown = false; // zero is a valid CRC, not an Unknown sentinel
    bool creationHashKnown = false;
    ResourceOrigin origin = ResourceOrigin::Unknown; // Custom has known-empty upstream candidates
    bool gpuOrdered = false; // real immediate GPU writer observed; NOT a CPU image/hash proof
    bool contentKnown = false; // false for GPU-only/no initial bytes/dirty writes
    D3D11_BUFFER_DESC buffer{};
    D3D11_TEXTURE2D_DESC texture{};
    static ResourceIdentity KnownNull() noexcept;
};

// Off-draw CPU/initialization operations only; includes descriptor bytes exactly
// as passed at creation, not GetDesc-normalized replacements. CRC32C is a product
// identity algorithm, not a deployment verifier. No external CRC library needed.
std::uint32_t ResourceCrc32c(std::uint32_t seed, const void* bytes, std::size_t count) noexcept;
bool HashBuffer(const D3D11_BUFFER_DESC&, CpuImage, std::uint32_t& dataHash, std::uint32_t& hash) noexcept;
bool HashTexture2D(const D3D11_TEXTURE2D_DESC&, CpuImage, const HashPolicy&,
                   std::uint32_t& dataHash, std::uint32_t& hash) noexcept;

// Embed one record in each retained resource lease. O(1) event calls, no maps,
// COM calls, locks, allocations, readback, logging, hashing, or upload. The same
// serial owner as the immediate context serializes event/snapshot access. Other
// writers must enqueue invalidations before the draw, not race this record.
class IdentityRecord final
{
    ResourceIdentity value_;
    HashPolicy policy_;
    std::uint32_t mapped_ = 0; // bounded write-map bits (single array slice)
    bool copiedDataPending_ = false;
    bool descriptorOnly_ = false; // never upgraded by copy/CPU refresh
public:
    // Only successful creates; object is a globally unique, never reused lease
    // ID, NOT a pointer. The legacy CreateBuffer hash path may hash a no-data
    // descriptor; this is distinct from CreateBufferDescriptor below, which
    // never calculates or certifies any upstream hash.
    bool CreateBuffer(std::uint64_t object, std::uint64_t deviceGeneration,
                      const D3D11_BUFFER_DESC&, CpuImage initial) noexcept;
    // Descriptor-only registration: no bytes/CRC, no stable-read/GPU proof.
    bool CreateBufferDescriptor(std::uint64_t object, std::uint64_t deviceGeneration,
                                const D3D11_BUFFER_DESC&) noexcept;
    bool CreateTexture2DDescriptor(std::uint64_t object, std::uint64_t deviceGeneration,
                                   const D3D11_TEXTURE2D_DESC&) noexcept;
    bool CreateTexture2D(std::uint64_t object, std::uint64_t deviceGeneration,
                         const D3D11_TEXTURE2D_DESC&, CpuImage initial, HashPolicy) noexcept;
    ResourceIdentity Snapshot() const noexcept { return value_; }
    void Destroy() noexcept;
    void OpaqueWrite() noexcept;
    void MapSucceeded(std::uint32_t subresource, D3D11_MAP mode) noexcept;
    void Unmap(std::uint32_t subresource) noexcept;
    void Update(std::uint32_t subresource) noexcept;
    void CopyResource(const IdentityRecord& source) noexcept;
    void CopySubresource(std::uint32_t destinationSubresource, const IdentityRecord& source,
                         std::uint32_t sourceSubresource, std::uint32_t x, std::uint32_t y,
                         std::uint32_t z, const D3D11_BOX* box) noexcept;
    // Optional full CPU snapshot revalidation at load/frame preparation ONLY.
    // Revision captured before copying CPU bytes rejects stale work. Upstream
    // buffer hashes and track_texture_updates=0 remain creation identities.
    bool RefreshCpu(std::uint64_t expectedContentGeneration, CpuImage) noexcept;
    // Off-draw descriptor append after exact full-copy propagation. Copy events
    // never calculate even descriptor CRCs. Until this returns true a tracked
    // destination's hash remains Unknown, and it cannot execute the component.
    bool FinalizeCopy(std::uint64_t expectedContentGeneration) noexcept;
};
} // namespace gimi
