#pragma once
#include <cstdint>
#include "CaptureOnlyConsumer.h"
#include "legacy/Il2CppCallSiteHook.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <memory>
#include "GenshinPresentLifecycle.h"
#include "GenshinResourceProvenance.h"

namespace GenshinLegacy
{
enum class CaptureOnlyMode : std::uint8_t { Disabled, ObservationOnly };
// Borrowed only for the producer callback on the owning render thread.
struct CaptureSource
{
    ID3D11Device* device=nullptr;
    ID3D11DeviceContext* context=nullptr;
    std::uint64_t deviceEpoch=0,ownerThread=0,ownerEpoch=0;
    std::uint64_t shaderHash=0,shaderIdentity=0,shaderDeviceEpoch=0;
    std::array<std::uint32_t,3> srvSlots{};
    std::array<ResourceProof::Witness,3> sourceWrites{};
    ResourceProof::Witness outputIdentity{};
    std::array<ID3D11Texture2D*,3> resources{}; // colour, raw engine depth, raw packed engine MV
    std::array<DXGI_FORMAT,3> viewFormats{};
};
using CaptureSignalProvider = bool (*)(const il2cpp_callsite::RenderToken&,
                                       const CaptureSource&,NoSrCapture::Signal&, void*) noexcept;
struct CapturePresentReceipt
{
    std::uint64_t device=0,deviceEpoch=0,swapchain=0,producerEpoch=0;
    std::uint64_t sourceFrame=0,renderGeneration=0,presentSerial=0;
    bool successfulNonTest=false;
};
// Must sample a completed successful non-TEST Present, never a predicted serial.
// The existing wrapper invokes OnPresent before its upcoming Present call.
using CapturePresentProvider = CapturePresentReceipt (*)(ID3D11Device*, void*) noexcept;

// Published only after the event query completes. Holding a snapshot prevents
// texture reuse; these are raw resource copies, not decoded NR/FG inputs.
struct CaptureSnapshot
{
    NoSrCapture::Signal signal{};
    std::uint64_t renderGeneration=0;
    std::array<std::uint64_t,3> sourceIdentity{};
    std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>,3> resources;
    bool gpuCompleted=false;
};

// The copied production hook stays dormant unless a reviewed producer publishes all metadata.
bool SetCaptureOnlyProviders(CaptureSignalProvider signalProvider,
                             CapturePresentProvider presentProvider,
                             void* opaque,CaptureOnlyMode mode=CaptureOnlyMode::Disabled) noexcept;
NoSrConsumer::Receipt GetCaptureOnlyReceipt() noexcept;
std::shared_ptr<const CaptureSnapshot> GetCaptureOnlySnapshot() noexcept;
std::uint64_t ResolveNativeSwapchainIdentity(IDXGISwapChain*) noexcept;
Presentation::Ticket BeginNativePresent(ID3D11Device*,std::uint64_t swapchain,UINT flags,bool present1) noexcept;
void CompleteNativePresent(const Presentation::Ticket&,HRESULT result) noexcept;
void ObserveNativePresentTarget(IDXGISwapChain*,std::uint64_t identity) noexcept;
void ForgetNativeSwapchain(std::uint64_t swapchain) noexcept;
Presentation::Receipt GetNativePresentReceipt() noexcept;
}
