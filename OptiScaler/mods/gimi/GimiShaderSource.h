#pragma once

#include "GimiDrawCache.h"
#include <d3d11shader.h>
#include <memory>
#include <vector>

namespace gimi
{
// Creation/Present preparation only. No shader-byte lookup, reflection,
// assembly, allocation or signature scan belongs in the draw executor.
inline constexpr std::size_t GimiShaderByteLimit = 1024u * 1024u;
inline constexpr std::size_t GimiShaderSourceBudget = 32u * 1024u * 1024u;
inline constexpr std::size_t GimiSignatureCapacity = 64;
struct ShaderSignatureParameter
{
    std::array<char, 65> semantic{};
    UINT semanticIndex = 0, registerIndex = 0, stream = 0;
    D3D_NAME systemValue = D3D_NAME_UNDEFINED;
    D3D_REGISTER_COMPONENT_TYPE component = D3D_REGISTER_COMPONENT_UNKNOWN;
    BYTE mask = 0, readWriteMask = 0;
    D3D_MIN_PRECISION precision = D3D_MIN_PRECISION_DEFAULT;
};
struct ShaderSignature
{
    std::array<ShaderSignatureParameter, GimiSignatureCapacity> inputs{}, outputs{};
    UINT inputCount = 0, outputCount = 0;
};
enum class ShaderSourceStatus : std::uint8_t
{
    Unknown, Ready, InvalidIdentity, UnsupportedModel, SizeLimit, BudgetLimit,
    ReflectionUnavailable, InvalidSignature, AllocationFailure
};
class ShaderSource;
struct ShaderSourceResult
{
    std::shared_ptr<const ShaderSource> source;
    ShaderSourceStatus status = ShaderSourceStatus::Unknown;
};
// Caller supplies the reflection obtained from these exact creation bytes.
// The original identity is never replaced by a transformed-byte identity.
ShaderSourceResult CaptureShaderSource(ShaderIdentity, ShaderModel, const void*, std::size_t,
    ID3D11ShaderReflection&, const D3D11_SHADER_DESC&, std::size_t availableBytes) noexcept;
class ShaderSource final
{
    friend ShaderSourceResult CaptureShaderSource(ShaderIdentity, ShaderModel, const void*, std::size_t,
        ID3D11ShaderReflection&, const D3D11_SHADER_DESC&, std::size_t) noexcept;
    ShaderIdentity original_;
    ShaderModel model_ = ShaderModel::Unknown;
    ShaderSignature signature_;
    std::vector<std::uint8_t> bytes_;
public:
    ShaderSource() = default;
    const auto& Original() const noexcept { return original_; }
    auto Model() const noexcept { return model_; }
    const auto& Signature() const noexcept { return signature_; }
    const auto& Bytes() const noexcept { return bytes_; }
    // Retained payload budget includes the bounded signature arrays. This is
    // not a process-heap accounting claim (shared_ptr allocator overhead aside).
    auto PayloadBytes() const noexcept { return sizeof(ShaderSource) + bytes_.capacity(); }
};

// A REQUESTED signature for the known TexFx ps_5_0 float4 o6 patch. Not proof
// of generated instructions, t120 loads, a shader object, MRT binding or pixels.
// Preserves EVERY original input/output descriptor and its order. Existing o6
// or SV_Target6 is refused, including partial/aliased declarations.
struct PixelTarget6Signature
{
    ShaderIdentity original;
    ShaderSignature expected;
    bool valid = false;
};
bool MatchesOriginalShaderSignature(const ShaderSource&, ID3D11ShaderReflection&) noexcept;
bool PreparePixelTarget6Signature(const ShaderSource&, PixelTarget6Signature&) noexcept;
// Invoke on the final assembled bytes' actual D3DReflect result, BEFORE
// CreatePixelShader/publication. Reflection alone never certifies instruction
// reads or GPU execution. Original identity/plan generation stay caller-owned.
bool MatchesPixelTarget6Signature(const PixelTarget6Signature&, ID3D11ShaderReflection&) noexcept;
} // namespace gimi
