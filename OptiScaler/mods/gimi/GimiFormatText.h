#pragma once

#include "GimiDx11Resources.h"
#include "GimiTextRender.h"
#include "GimiNativeOverlay.h"

namespace gimi
{
inline constexpr unsigned NativeFormatTextDispatches = 8;
class FormatTextLibrary final
{
public:
    struct Shader { Microsoft::WRL::ComPtr<ID3D11ComputeShader> object; };
    std::shared_ptr<const Plan> plan;
    std::vector<Shader> shaders;
};
// Load/Present preparation only: explicit admitted source, main/cs_5_0,
// COMPUTE_SHADER macro, bounded bytes and exact reflected resource ABI.
std::shared_ptr<const FormatTextLibrary> CreateFormatTextLibrary(
    std::shared_ptr<const Plan>, ID3D11Device*, std::vector<Diagnostic>&);

class NativeComputePacket final
{
    friend class NativeComputeExecutor;
    struct Storage;
    std::unique_ptr<Storage> state_;
    NativeComputePacket(std::uint32_t actions, ID3D11Device*,
                        std::shared_ptr<const Dx11StaticResources>, std::shared_ptr<const FormatTextLibrary>,
                        std::shared_ptr<const NativeTextShaderLibrary>);
public:
    ~NativeComputePacket();
    NativeComputePacket(const NativeComputePacket&) = delete;
    NativeComputePacket& operator=(const NativeComputePacket&) = delete;
    const Plan* Commands() const noexcept;
    const Dx11AliasChange* Aliases() const noexcept;
    UINT AliasCount() const noexcept;
    std::uint64_t ReaderEpoch() const noexcept; // same actual ticket for the complete mixed tape
    UINT DispatchCount() const noexcept;
    const Dx11RetainedBinding* Clears() const noexcept;
    UINT ClearCount() const noexcept;
    bool ParameterCommit() const noexcept;
    UINT FinalParameters() const noexcept;
    void Retire() noexcept; // only after cancellation/no GPU work or actual fence completion
};
struct NativeComputeResult
{
    bool success = false, consumed = false; // consumed means a GPU operation MAY have been issued
    const char* reason = "Native compute unavailable"; // fixed memory-only diagnostic, no hook IO
};
class NativeComputeExecutor final
{
    std::vector<std::shared_ptr<NativeComputePacket>> flights_;
    ID3D11Device* device_ = nullptr; // retained by every packet/context's real Session owner
public:
    NativeComputeExecutor(std::uint32_t actions, unsigned flights, ID3D11Device*,
                          std::shared_ptr<const Dx11StaticResources>, std::shared_ptr<const FormatTextLibrary>,
                          std::shared_ptr<const NativeTextShaderLibrary>);
    // ONE complete ordered PRE (finite parameters/text/overlay) or POST tape
    // before any GPU effect. Finite page changes commit with the full alias
    // journal; dynamic coordinates must restore. No component can submit a prefix. Requires the
    // serial real immediate owner and existing internal-hook suppression.
    // Backbuffer overlays require the actual native PRE ticket as nativeEpoch;
    // the whole packet's independent reader is reserved under that ticket before Submit.
    // The caller MUST reserve/retain the packet before Submit. Capacity failure
    // does not wait and cannot allocate or release a submitted flight early.
    std::shared_ptr<NativeComputePacket> Prepare(ID3D11DeviceContext*, IDXGISwapChain*, HWND,
                                               const BatchView&, std::uint32_t parameterState,
                                               const char*& failure, Dx11ResourceProvider* privateTargets = nullptr,
                                               std::uint64_t nativeEpoch = 0, const OptiInput::GameCursorSnapshot* cursor = nullptr) noexcept;
    NativeComputeResult Submit(ID3D11DeviceContext*, NativeComputePacket&, std::uint64_t nativeEpoch = 0) noexcept;
};
} // namespace gimi
