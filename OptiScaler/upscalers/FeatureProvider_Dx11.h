#pragma once

#include "SysUtils.h"

#include "IFeature_Dx11.h"

#include <inputs/NVNGX_DLSS.h>

class FeatureProvider_Dx11
{
  public:
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

    static bool GetFeature(Upscaler upscaler, UINT handleId, NVSDK_NGX_Parameter* parameters,
                           std::unique_ptr<IFeature_Dx11>* feature, bool preserveDx12Fallback);

    static bool ChangeFeature(Upscaler upscaler, ID3D11Device* device, ID3D11DeviceContext* cmdList, UINT handleId,
                              NVSDK_NGX_Parameter* parameters, ContextData<IFeature_Dx11>* contextData,
                              bool preserveDx12Fallback);
};
