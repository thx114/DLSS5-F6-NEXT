#pragma once

#include "GimiCommands.h"
#include "GimiDebugOverlay.h"
#include "GimiIdentity.h"
#include "GimiIniParams.h"
#include <wrl/client.h>
#include <atomic>

namespace gimi
{
struct PreloadLimits
{
    std::uint64_t perFileBytes = 256ull * 1024 * 1024;
    std::uint64_t totalBytes = 512ull * 1024 * 1024;
};
struct StaticResource
{
    ResourceKind kind = ResourceKind::Alias;
    std::vector<std::uint8_t> bytes;
    struct Mip { std::size_t offset; std::uint32_t rowPitch, slicePitch; };
    std::vector<Mip> mips;
    D3D11_BUFFER_DESC buffer{};
    D3D11_TEXTURE2D_DESC texture{};
    DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN;
    std::uint32_t stride = 0;
};
class ResourceFiles final
{
    friend struct ResourceFileLoader;
    std::shared_ptr<const Plan> plan_;
    std::vector<StaticResource> resources_;
    IniParameterSpec parameters_;
    std::uint64_t totalBytes_ = 0, byteLimit_ = 0;
public:
    const Plan& Commands() const noexcept { return *plan_; }
    const auto& PlanOwner() const noexcept { return plan_; }
    const auto& Resources() const noexcept { return resources_; }
    const auto& Parameters() const noexcept { return parameters_; }
    std::uint64_t NativePageCapacity() const noexcept
    { return byteLimit_ >= totalBytes_ ? (byteLimit_ - totalBytes_) / (IniParameterScalars * sizeof(float)) : 0; }
};
struct FilePreloadResult
{
    std::shared_ptr<const ResourceFiles> files;
    std::vector<Diagnostic> diagnostics;
    explicit operator bool() const noexcept { return files != nullptr; }
};
// Full required set or nothing. Proves a finite parameter-value closure before
// creating shader-visible immutable pages; unknown/unbounded input is rejected
// at its actual command/expression source. No CPU-only parameter fallback.
FilePreloadResult PreloadResources(std::shared_ptr<const Plan> plan, PreloadLimits limits = {});

struct Dx11LeaseView
{
    ID3D11Resource* resource = nullptr; // borrowed, retained by Dx11StaticResources
    ID3D11Buffer* buffer = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    Binding binding;
    ResourceIdentity identity;
    // Actual format/device proof, never a filename/shader declaration. Set by
    // static texture preparation or the separately owned Present private-texture factory.
    bool sampleableFloat2D = false;
    ID3D11UnorderedAccessView* uav = nullptr; // only declared structured formatter output
};
class Dx11StaticResources final
{
    friend struct Dx11ResourceLoader;
    struct Lease
    {
        Microsoft::WRL::ComPtr<ID3D11Resource> resource;
        Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;
        Binding binding;
        ResourceIdentity identity;
        bool sampleableFloat2D = false;
    };
    std::shared_ptr<const ResourceFiles> files_;
    std::vector<Lease> leases_;
    struct ParameterPage
    {
        Microsoft::WRL::ComPtr<ID3D11Texture1D> texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    };
    std::vector<ParameterPage> parameters_;
    mutable std::atomic<unsigned> nativeParameterPages_{0};
    std::shared_ptr<const DebugOverlayLibrary> debugOverlay_;
    std::array<Microsoft::WRL::ComPtr<ID3D11RasterizerState>, 2> raster_;
    std::uint64_t firstLease_ = 0, deviceGeneration_ = 0;
public:
    // O(1), no AddRef/GetDesc/GetResource/IO/hash/allocation or upload.
    ID3D11RasterizerState* Raster(unsigned n) const noexcept { return n && n <= raster_.size() ? raster_[n - 1].Get() : nullptr; }
    const auto& DebugOverlay() const noexcept { return debugOverlay_; }
    bool ClaimNativeParameterPage() const noexcept
    {
        auto count = nativeParameterPages_.load(std::memory_order_relaxed);
        for (;;)
        {
            if (parameters_.size() >= IniParameterPageLimit || count >= IniParameterPageLimit - parameters_.size() ||
                count >= files_->NativePageCapacity()) return false;
            if (nativeParameterPages_.compare_exchange_weak(count, count + 1, std::memory_order_relaxed)) return true;
        }
    }
    void ReleaseNativeParameterPages(unsigned count) const noexcept
    { if (count) nativeParameterPages_.fetch_sub(count, std::memory_order_relaxed); }
    bool Resolve(Binding binding, Dx11LeaseView& out) const noexcept;
    bool Get(Id resource, Dx11LeaseView& out) const noexcept;
    bool Initialize(CommandState& state) const noexcept;
    // Page selection is a finite-state CPU operation. Returned SRV is borrowed
    // from this owner; the submitted packet retains the entire immutable set.
    ID3D11ShaderResourceView* ParameterView(std::uint32_t state) const noexcept
    { return state < parameters_.size() ? parameters_[state].srv.Get() : nullptr; }
    bool ReadParameter(std::uint32_t state, Id scalar, float& value) const noexcept
    { return files_->Parameters().Read(state, scalar, value); }
    bool WriteParameter(std::uint32_t state, Id scalar, float value, std::uint32_t& next) const noexcept
    { return files_->Parameters().Write(state, scalar, value, next); }
    std::uint64_t DeviceGeneration() const noexcept { return deviceGeneration_; }
    const auto& Files() const noexcept { return files_; }
};
struct Dx11PreloadResult
{
    std::shared_ptr<const Dx11StaticResources> resources;
    std::vector<Diagnostic> diagnostics;
    HRESULT deviceResult = S_OK;
    explicit operator bool() const noexcept { return resources != nullptr; }
};
// Initialization only, on the device owner's safe thread, internal-hook
// suppression active. firstLease reserves [firstLease, firstLease+count) in the
// runtime's GLOBAL never-reused lease ID domain. Publish only after full success.
// Hold the returned set through alias references, submissions and GPU retirement;
// destruction releases COM objects but DOES NOT prove GPU completion.
Dx11PreloadResult CreateDx11Resources(std::shared_ptr<const ResourceFiles> files,
                                    ID3D11Device* device, std::uint64_t deviceGeneration,
                                    std::uint64_t firstLease, HashPolicy policy = {});
} // namespace gimi
