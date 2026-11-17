// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <OptiTypes.h>
#include <atomic>
#include <cstdint>
#include <optional>

namespace BackendSelection
{
inline std::atomic<uint64_t> dx11Revision { 0 };
inline uint64_t Dx11Revision() noexcept { return dx11Revision.load(std::memory_order_acquire); }

// Only an explicit Apply action commits a preference. Construction and fallback
// report the actual provider through the feature, without changing this request.
template<class Config>
void Commit(API api, Upscaler requested, Config& config)
{
    if (requested == Upscaler::Reset) return;
    switch (api)
    {
    case API::DX11:
        config.Dx11Upscaler = requested;
        dx11Revision.fetch_add(1, std::memory_order_release);
        break;
    case API::DX12: config.Dx12Upscaler = requested; break;
    case API::Vulkan: config.VulkanUpscaler = requested; break;
    default: break;
    }
}

constexpr Upscaler ResolveDx11(std::optional<Upscaler> requested, bool taaInput,
    std::optional<Upscaler> dx12Preference, bool dlssCapable, bool nativeNgxReady)
{
    if (requested) return *requested;
    if (taaInput && dx12Preference)
    {
        switch (*dx12Preference)
        {
        case Upscaler::DLSS: if (dlssCapable) return Upscaler::DLSS_on12; break;
        case Upscaler::XeSS: return Upscaler::XeSS_on12;
        case Upscaler::FSR21: return Upscaler::FSR21_on12;
        case Upscaler::FSR22: return Upscaler::FSR22_on12;
        case Upscaler::FFX: return Upscaler::FFX_on12;
        default: break;
        }
    }
    return dlssCapable && nativeNgxReady ? Upscaler::DLSS : Upscaler::FSR22;
}
}
