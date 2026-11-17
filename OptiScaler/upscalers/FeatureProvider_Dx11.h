#pragma once

#include "SysUtils.h"

#include "IFeature_Dx11.h"

#include <inputs/NVNGX_DLSS.h>

class FeatureProvider_Dx11
{
  public:
    // Requested auto remains auto. Only the synthetic TAA source may inherit a
    // compatible DX12 preference; native game NGX keeps its original policy.
    static constexpr Upscaler ResolveBackend(std::optional<Upscaler> requested, bool taaInput,
        std::optional<Upscaler> dx12Preference, bool dlssCapable, bool nativeNgxReady)
    {
        if (requested) return *requested;
        if (taaInput && dx12Preference == Upscaler::DLSS && dlssCapable) return Upscaler::DLSS_on12;
        return dlssCapable && nativeNgxReady ? Upscaler::DLSS : Upscaler::FSR22;
    }

    // Preserve only an explicit bridge request. Native and unknown requests
    // retain their existing D3D11 fallback; consumer settings grant no new route.
    static constexpr Upscaler FallbackUpscaler(Upscaler requested, bool preserveDx12)
    {
        if (preserveDx12)
        {
            switch (requested)
            {
            case Upscaler::XeSS_on12:
            case Upscaler::FSR21_on12:
            case Upscaler::FSR22_on12:
            case Upscaler::FFX_on12:
            case Upscaler::DLSS_on12:
                return Upscaler::FSR22_on12;
            default:
                break;
            }
        }
        return Upscaler::FSR22;
    }

    // Initialization can create a transport before currentFeature is published.
    // Policies use this provider identity, including any actual fallback.
    static bool InitializeFeature(IFeature_Dx11* feature,ID3D11Device* device,ID3D11DeviceContext* context,
                                  NVSDK_NGX_Parameter* parameters);
    static std::optional<Upscaler> ActiveBackend(ID3D11Device* sourceDevice=nullptr);
    static void BindActiveFeature(IFeature_Dx11* feature,ID3D11DeviceContext* context);
    static void ForgetFeature(const IFeature_Dx11* feature);

    static bool GetFeature(Upscaler upscaler, UINT handleId, NVSDK_NGX_Parameter* parameters,
                           std::unique_ptr<IFeature_Dx11>* feature, bool preserveDx12Fallback);

    static bool ChangeFeature(Upscaler upscaler, ID3D11Device* device, ID3D11DeviceContext* cmdList, UINT handleId,
                              NVSDK_NGX_Parameter* parameters, ContextData<IFeature_Dx11>* contextData,
                              bool preserveDx12Fallback);
};
