#pragma once
#include "SysUtils.h"
#include <NVNGX_Parameter.h>
#include <upscalers/IFeature_Dx12.h>
#include <framegen/dlssg/DlssgInputContinuity.h>
#include <framegen/dlssg/DLSSG_Dx12.h>

class UpscalerInputsDx12
{
  private:
    inline static ID3D12Device* _device = nullptr;

  public:
    static void Init(ID3D12Device* device);
    static void Reset();
    static dlssg::NativeSource UpscaleStart(ID3D12GraphicsCommandList* InCmdList, NVSDK_NGX_Parameter* InParameters,
                             IFeature_Dx12* feature);
    static void UpscaleEnd(ID3D12GraphicsCommandList* InCmdList, NVSDK_NGX_Parameter* InParameters,
                           IFeature_Dx12* feature, bool succeeded=true,
                           const DLSSG_Dx12::UpscalerColourSourcePtr& colourSource={},
                           ID3D12Resource* evaluatedOutput=nullptr);
};
