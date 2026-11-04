#include <pch.h>
#include "DLSSFeature_Dx12.h"
#include <genshin/GenshinFsrSubstageProgress.h>
#include <genshin/GenshinSrReceipt.h>
#include <dxgi1_4.h>
#include <Config.h>
#include <upscalers/FeatureLifecycleTrace.h>

bool DLSSFeatureDx12::InitInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    if (IsInited())
        return true;

    return InitDLSS(InCommandList, InParameters);
}

bool DLSSFeatureDx12::InitDLSS(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    if (_routedRelease && _nativeRelease.HoldsOutput())
        return false; // no overwrite of a live/uncertain native owner on Init retry
    if (NVNGXProxy::NVNGXModule() == nullptr)
    {
        LOG_ERROR("nvngx.dll not loaded!");
        return false;
    }

    if (!_dlssInitedDx12)
    {
        _dlssInitedDx12 = NVNGXProxy::InitDx12(Device);

        if (!_dlssInitedDx12)
            return false;

        _moduleLoaded =
            (NVNGXProxy::D3D12_Init_ProjectID() != nullptr || NVNGXProxy::D3D12_Init_Ext() != nullptr) &&
            NVNGXProxy::HasD3D12Shutdown() &&
            (NVNGXProxy::D3D12_GetParameters() != nullptr || NVNGXProxy::D3D12_AllocateParameters() != nullptr) &&
            NVNGXProxy::D3D12_DestroyParameters() != nullptr && NVNGXProxy::D3D12_CreateFeature() != nullptr &&
            NVNGXProxy::D3D12_ReleaseFeature() != nullptr && NVNGXProxy::D3D12_EvaluateFeature() != nullptr;

        // delay between init and create feature
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    LOG_INFO("Creating DLSS feature");

    if (NVNGXProxy::D3D12_CreateFeature() != nullptr)
    {
        ProcessInitParams(InParameters);

        // Routed acquisition starts empty. The legacy inline placeholder is
        // not evidence that native Create ever acquired a feature.
        _p_dlssHandle = _routedRelease ? nullptr : &_dlssHandle;

        NVSDK_NGX_Result nvResult;
        {
            ScopedSkipHeapCapture skipHeapCapture {};

            FeatureLifecycleTrace providerCreate("provider D3D12 CreateFeature");
            nvResult = NVNGXProxy::D3D12_CreateFeature()(InCommandList, NVSDK_NGX_Feature_SuperSampling, InParameters,
                                                         &_p_dlssHandle);
        }

        if (_routedRelease)
            nvResult = _nativeRelease.ObserveCreate(nvResult, _p_dlssHandle);

        if (nvResult != NVSDK_NGX_Result_Success)
        {
            LOG_ERROR("_CreateFeature result: {0:X}", (unsigned int) nvResult);
            return false;
        }
        else
        {
            LOG_INFO("_CreateFeature result: NVSDK_NGX_Result_Success");
        }
    }
    else
    {
        LOG_ERROR("_CreateFeature is nullptr");
        return false;
    }

    ReadVersion();

    SetInit(true);
    return true;
}

bool DLSSFeatureDx12::EvaluateInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    if (_routedRelease && !_nativeRelease.Acquired())
        return false;
    if (!_moduleLoaded)
    {
        GenshinSubstage::Event(GenshinSubstage::Stage::ProviderAttempt,GenshinSubstage::State::Failed,
            GenshinSubstage::Resource::Unknown,GenshinSubstage::Domain::Ngx,0,GenshinSubstage::Issue::NoProvider);
        LOG_ERROR("nvngx.dll or _nvngx.dll is not loaded!");
        return false;
    }

    NVSDK_NGX_Result nvResult;

    if (NVNGXProxy::D3D12_EvaluateFeature() != nullptr)
    {
        ProcessEvaluateParams(InParameters);

        auto* outputScope=GenshinSrReceipt::active;
        if(outputScope)outputScope->BeforeDlss12(InCommandList,InParameters,RenderWidth(),RenderHeight(),TargetWidth(),TargetHeight());
        GenshinSubstage::ActualProviderCall providerCall;
        auto* const transportScope = GenshinTransport::Scope::Current();
        const bool recordingCpuEnabled = outputScope && transportScope && transportScope->lease &&
            transportScope->parameters == InParameters && transportScope->owner.thread == GetCurrentThreadId();
        GenshinSrRecordingCpu::Scope ngxCpu(static_cast<IFeature_Dx12*>(this), GenshinSrRecordingCpu::Stage::NgxEvaluate, recordingCpuEnabled);
        nvResult = NVNGXProxy::D3D12_EvaluateFeature()(InCommandList, _p_dlssHandle, InParameters, NULL);
        ngxCpu.Finish(nvResult == NVSDK_NGX_Result_Success ? S_OK : E_FAIL);
        providerCall.Returned(static_cast<uint32_t>(nvResult),nvResult==NVSDK_NGX_Result_Success);
        if(outputScope)outputScope->Dlss12Returned(nvResult==NVSDK_NGX_Result_Success);

        if (nvResult != NVSDK_NGX_Result_Success)
        {
            LOG_ERROR("_EvaluateFeature result: {0:X}", (unsigned int) nvResult);
            return false;
        }
    }
    else
    {
        GenshinSubstage::Event(GenshinSubstage::Stage::ProviderAttempt,GenshinSubstage::State::Failed,
            GenshinSubstage::Resource::Unknown,GenshinSubstage::Domain::Ngx,0,GenshinSubstage::Issue::NoProvider);
        LOG_ERROR("_EvaluateFeature is nullptr");
        return false;
    }

    _frameCount++;

    return true;
}

void DLSSFeatureDx12::Shutdown(ID3D12Device* InDevice)
{
    if (_dlssInitedDx12)
    {
        if (NVNGXProxy::D3D12_Shutdown() != nullptr)
            NVNGXProxy::D3D12_Shutdown()();
        else if (NVNGXProxy::D3D12_Shutdown1() != nullptr)
            NVNGXProxy::D3D12_Shutdown1()(InDevice);
    }

    DLSSFeature::Shutdown();
}

DLSSFeatureDx12::DLSSFeatureDx12(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters)
    : IFeature(InHandleId, InParameters), IFeature_Dx12(InHandleId, InParameters), DLSSFeature(InHandleId, InParameters)
{
    // Capture actual selected ingress, not mutable EnableDlssInputs. Unrelated
    // virtual/bridge features and other games retain their legacy lifecycle.
    _routedRelease = NgxInput::SelectedInputCall();
    if (NVNGXProxy::NVNGXModule() == nullptr)
    {
        LOG_INFO("nvngx.dll not loaded, now loading");
        NVNGXProxy::InitNVNGX();
    }

    LOG_INFO("binding complete!");
}

NVSDK_NGX_Result DLSSFeatureDx12::TryRelease()
{
    if (!_routedRelease)
        return NVSDK_NGX_Result_Success;
    const auto result = _nativeRelease.TryRelease(_p_dlssHandle, NVNGXProxy::D3D12_ReleaseFeature());
    if (result == NVSDK_NGX_Result_Success)
        SetInit(false);
    return result;
}

DLSSFeatureDx12::~DLSSFeatureDx12()
{
    if (State::Instance().isShuttingDown)
        return;

    if (_routedRelease)
    {
        // Input Release and the sole ChangeFeature caller preflight while the
        // complete object is still owned. An already-released state is a no-op.
        const auto result = TryRelease();
        if (result != NVSDK_NGX_Result_Success)
            LOG_ERROR("Routed DLSS destructor reached without successful release preflight: {:X}", (unsigned) result);
        return;
    }

    if (NVNGXProxy::D3D12_ReleaseFeature() != nullptr && _p_dlssHandle != nullptr)
    {
        FeatureLifecycleTrace providerRelease("provider D3D12 ReleaseFeature", _p_dlssHandle->Id);
        NVNGXProxy::D3D12_ReleaseFeature()(_p_dlssHandle);
    }
}
