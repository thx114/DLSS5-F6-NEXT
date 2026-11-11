#pragma once
#include "DLSSFeature.h"
#include <upscalers/IFeature_Dx12.h>
#include <shaders/rcas/RCAS_Dx12.h>
#include <string>
#include <hooks/ngx/SelectedFeatureRelease.h>

class DLSSFeatureDx12 : public DLSSFeature, public IFeature_Dx12
{
  private:
    bool _routedRelease = false;
    NgxInput::NativeFeatureRelease _nativeRelease;
  protected:
    bool InitDLSS(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters);

  public:
    NVSDK_NGX_Result TryRelease() override;
    bool InitInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters) override;
    bool EvaluateInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters) override;

    static void Shutdown(ID3D12Device* InDevice);

    feature_version Version() override { return DLSSFeature::Version(); }
    Upscaler GetUpscalerType() const final { return DLSSFeature::GetUpscalerType(); }
    API Api() const override { return IFeature_Dx12::Api(); }
    bool CallsUpscalerEndByItself() override { return IFeature_Dx12::CallsUpscalerEndByItself(); }

    bool IsWithDx12() override { return false; }

    DLSSFeatureDx12(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters);
    ~DLSSFeatureDx12();
};
