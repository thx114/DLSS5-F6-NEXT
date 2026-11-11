#pragma once

#include <d3d11_1.h>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace gimi
{
struct BufferResizeRule
{
    std::uint32_t hash = 0, byteWidth = 0, vertexCount = 0, byteStride = 0, uavStride = 0;
};
// Creation-only scratch owns any padded initial bytes through the real API.
struct BufferCreationEdit
{
    D3D11_BUFFER_DESC original{}, effective{};
    D3D11_SUBRESOURCE_DATA initial{};
    BufferResizeRule rule{};
    std::vector<std::uint8_t> padded;
    std::uint64_t deviceGeneration = 0, revision = 0;
    std::uint32_t creationHash = 0;
    bool qualified = false, changed = false;
};
struct SoElement
{
    UINT stream = 0, semanticIndex = 0;
    BYTE start = 0, count = 0, output = 0;
    bool gap = false;
    std::array<char, 65> semantic{};
};
// From the ORIGINAL successful shader creation. Never replaces the shader,
// stream-output bindings or the driver's hidden append cursor.
struct Dx11StreamOutputContract
{
    bool qualified = false, vertexPassthrough = false;
    UINT entryCount = 0, strideCount = 0, slots = 0;
    UINT rasterizedStream = D3D11_SO_NO_RASTERIZED_STREAM;
    UINT maxVertices = 0, instances = 0;
    std::array<SoElement, 128> elements{};
    std::array<UINT, D3D11_SO_BUFFER_SLOT_COUNT> strides{};
};
class Dx11GameLease;
struct Dx11StreamOutputTarget
{
    std::shared_ptr<const Dx11GameLease> owner;
    ID3D11Buffer* buffer = nullptr;
    D3D11_BUFFER_DESC descriptor{};
    std::uint64_t object = 0, deviceGeneration = 0, cursorUpper = 0;
    // The producer packet snapshots the pre-submit write generation. It is
    // checked again by the serial owner before reservation and publication.
    // A cursor is only an upper byte bound, never the driver's hidden counter.
    std::uint64_t contentGeneration = 0;
};
struct Dx11StreamOutputBinding
{
    std::array<Dx11StreamOutputTarget, D3D11_SO_BUFFER_SLOT_COUNT> targets{};
    std::uint64_t revision = 0;
    UINT mask = 0;
};
} // namespace gimi
