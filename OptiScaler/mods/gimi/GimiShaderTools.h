#pragma once
#include "GimiShaderSource.h"
#include <wrl/client.h>
#include <utility>

namespace gimi
{
struct IniParameterRead { std::uint32_t vector = 0; std::uint8_t components = 0; };
class ShaderRegexLibrary;
struct PreparedPixelShader;
// Owning capability for a source-preserving typed VS rewrite. Added CB slots
// describe the FINAL code, not stale RDEF (which intentionally stays original).
class Dx11VertexReplacement final
{
    friend class ShaderRegexLibrary;
    std::shared_ptr<const ShaderSource> source_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> original_, shader_;
    std::array<IniParameterRead, 256> reads_{};
    std::array<UINT, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT> addedConstantBytes_{}, requiredConstantBytes_{};
    std::uint32_t readCount_ = 0;
    std::uint64_t plan_ = 0, lease_ = 0;
    std::string sourceLabel_;
    std::array<std::string, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT> constantLabels_;
public:
    const auto& SourceLabel() const noexcept { return sourceLabel_; }
    const auto& ConstantLabel(unsigned slot) const noexcept
    { return constantLabels_[slot].empty() ? sourceLabel_ : constantLabels_[slot]; }
    ID3D11VertexShader* Shader() const noexcept { return shader_.Get(); }
    const auto& Reads() const noexcept { return reads_; }
    auto ReadCount() const noexcept { return readCount_; }
    const auto& AddedConstantBytes() const noexcept { return addedConstantBytes_; }
    const auto& RequiredConstantBytes() const noexcept { return requiredConstantBytes_; }
    bool ValidFor(ID3D11Device*, ID3D11VertexShader* original, ShaderIdentity,
                  std::uint64_t plan, std::uint64_t deviceGeneration) const noexcept;
};
struct PreparedVertexShader
{
    ShaderEntryLink link;
    std::shared_ptr<const Dx11VertexReplacement> replacement;
    std::vector<Diagnostic> diagnostics;
    explicit operator bool() const noexcept { return link.valid; }
};
// Capability produced ONLY by the Present toolchain after actual assembled
// code/signature checks and successful authentic device CreatePixelShader.
class Dx11PixelReplacement final
{
    friend class ShaderRegexLibrary;
    std::shared_ptr<const ShaderSource> source_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> original_, shader_;
    std::array<IniParameterRead, 256> reads_{};
    std::uint32_t readCount_ = 0;
    std::uint64_t plan_ = 0, lease_ = 0;
    bool diffuseResources_ = false; // genuine float4 Target6 + exact TexFx instructions
    bool outlineResources_ = false; // exact content-qualified template only
public:
    Dx11PixelReplacement() = default;
    ID3D11PixelShader* Shader() const noexcept { return shader_.Get(); }
    const auto& Reads() const noexcept { return reads_; }
    auto ReadCount() const noexcept { return readCount_; }
    // Requires real t69/t70 material witnesses and the actual current s15.
    // Not a declaration-only resource/sampler authorization.
    bool OutlineResources() const noexcept { return outlineResources_; }
    bool DiffuseResources() const noexcept { return diffuseResources_; }
    bool ValidFor(ID3D11Device*, ID3D11PixelShader* original, ShaderIdentity,
                  std::uint64_t plan, std::uint64_t deviceGeneration) const noexcept;
};
struct PreparedPixelShader
{
    ShaderEntryLink link;
    std::shared_ptr<const Dx11PixelReplacement> replacement;
    std::vector<Diagnostic> diagnostics;
    explicit operator bool() const noexcept { return link.valid; }
};
// Immutable compiled PCRE2 codes, owned by the exact immutable Plan. No JIT,
// runtime code cache or disk IO. Compile/load and Prepare are off-draw only.
class ShaderRegexLibrary final
{
    struct Impl;
    struct Prepared;
    Prepared PrepareStage(std::shared_ptr<const ShaderSource>, ShaderIdentity, ShaderModel,
        ID3D11Device*, ID3D11DeviceChild*, std::uint64_t plan, std::uint64_t lease, bool vertex) const;
    std::shared_ptr<const Impl> impl_;
    explicit ShaderRegexLibrary(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}
public:
    static std::shared_ptr<const ShaderRegexLibrary> Compile(std::shared_ptr<const Plan>, std::vector<Diagnostic>&);
    // Caller MUST enter the existing real hook internal suppression before
    // either preparation (created replacements must not register as GIMI originals).
    PreparedPixelShader Prepare(std::shared_ptr<const ShaderSource>, ShaderIdentity original, ShaderModel,
        ID3D11Device*, ID3D11PixelShader* originalShader, std::uint64_t planGeneration,
        std::uint64_t replacementLease) const;
    PreparedVertexShader PrepareVertex(std::shared_ptr<const ShaderSource>, ShaderIdentity original, ShaderModel,
        ID3D11Device*, ID3D11VertexShader* originalShader, std::uint64_t planGeneration,
        std::uint64_t replacementLease) const;
};
} // namespace gimi
