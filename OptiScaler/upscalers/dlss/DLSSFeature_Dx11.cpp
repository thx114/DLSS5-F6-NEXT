#include <pch.h>
#include "DLSSFeature_Dx11.h"
#include <Config.h>
#include <genshin/GenshinFsrSubstageProgress.h>
#include <genshin/GenshinSrReceipt.h>

#include <dxgi.h>

bool DLSSFeatureDx11::InitInternal(ID3D11DeviceContext* InContext, NVSDK_NGX_Parameter* InParameters)
{
    if (NVNGXProxy::NVNGXModule() == nullptr)
    {
        LOG_ERROR("nvngx.dll not loaded!");

        SetInit(false);
        return false;
    }

    NVSDK_NGX_Result nvResult;
    bool initResult = false;

    do
    {
        if (!_dlssInitedDx11)
        {
            _dlssInitedDx11 = NVNGXProxy::InitDx11(Device);

            if (!_dlssInitedDx11)
                return false;

            _moduleLoaded =
                (NVNGXProxy::D3D11_Init_ProjectID() != nullptr || NVNGXProxy::D3D11_Init_Ext() != nullptr) &&
                (NVNGXProxy::D3D11_Shutdown() != nullptr || NVNGXProxy::D3D11_Shutdown1() != nullptr) &&
                (NVNGXProxy::D3D11_GetParameters() != nullptr || NVNGXProxy::D3D11_AllocateParameters() != nullptr) &&
                NVNGXProxy::D3D11_DestroyParameters() != nullptr && NVNGXProxy::D3D11_CreateFeature() != nullptr &&
                NVNGXProxy::D3D11_ReleaseFeature() != nullptr && NVNGXProxy::D3D11_EvaluateFeature() != nullptr;

            // delay between init and create feature
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        LOG_INFO("Creating DLSS feature");

        if (NVNGXProxy::D3D11_CreateFeature() != nullptr)
        {
            ProcessInitParams(InParameters);

            _p_dlssHandle = &_dlssHandle;
            nvResult = NVNGXProxy::D3D11_CreateFeature()(InContext, NVSDK_NGX_Feature_SuperSampling, InParameters,
                                                         &_p_dlssHandle);

            if (nvResult != NVSDK_NGX_Result_Success)
            {
                LOG_ERROR("NVNGXProxy::D3D11_CreateFeature result: {0:X}", (unsigned int) nvResult);
                break;
            }
        }
        else
        {
            LOG_ERROR("NVNGXProxy::D3D11_CreateFeature is nullptr");
            break;
        }

        ReadVersion();

        initResult = true;

    } while (false);

    SetInit(initResult);

    return initResult;
}

bool DLSSFeatureDx11::EvaluateInternal(ID3D11DeviceContext* InDeviceContext, NVSDK_NGX_Parameter* InParameters)
{
    if (!_moduleLoaded)
    {
        GenshinSubstage::Event(GenshinSubstage::Stage::ProviderAttempt, GenshinSubstage::State::Failed,
            GenshinSubstage::Resource::Unknown, GenshinSubstage::Domain::Ngx, 0, GenshinSubstage::Issue::NoProvider);
        LOG_ERROR("nvngx.dll or _nvngx.dll is not loaded!");
        return false;
    }

    NVSDK_NGX_Result nvResult;

    if (NVNGXProxy::D3D11_EvaluateFeature() != nullptr)
    {
        ProcessEvaluateParams(InParameters);

        // Observe final parameters after ProcessEvaluateParams has applied all
        // actual overrides. Missing Get values remain explicitly unknown.
        static std::atomic<uint32_t> providerDiagnosticSamples{0};
        if(GenshinSrReceipt::active && providerDiagnosticSamples.load(std::memory_order_relaxed)<32 &&
            providerDiagnosticSamples.fetch_add(1,std::memory_order_relaxed)<32) {
            const auto id=GenshinSrReceipt::active->Snapshot().identity;
            float jx=0,jy=0,mx=0,my=0;int reset=0;unsigned int width=0,height=0,outWidth=0,outHeight=0,subrectWidth=0,subrectHeight=0;
            const auto gx=InParameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_X,&jx);
            const auto gy=InParameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y,&jy);
            const auto gmx=InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_X,&mx);
            const auto gmy=InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_Y,&my);
            const auto gr=InParameters->Get(NVSDK_NGX_Parameter_Reset,&reset);
            const auto gw=InParameters->Get(NVSDK_NGX_Parameter_Width,&width);
            const auto gh=InParameters->Get(NVSDK_NGX_Parameter_Height,&height);
            const auto gow=InParameters->Get(NVSDK_NGX_Parameter_OutWidth,&outWidth);
            const auto goh=InParameters->Get(NVSDK_NGX_Parameter_OutHeight,&outHeight);
            const auto gsw=InParameters->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,&subrectWidth);
            const auto gsh=InParameters->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,&subrectHeight);
            LOG_INFO("Genshin SR32 native provider: pid={} tid={} tick={} instance={} generation={} sourceFrame={} context={} jitter=({}, {}) jitterKnown=({}, {}) mvScale=({}, {}) mvKnown=({}, {}) reset={} resetKnown={} ngx={}x{}->{}x{} sizeKnown=({}, {}, {}, {}) subrect={}x{} subrectKnown=({}, {}) featureRender={}x{} target={}x{} display={}x{}",
                GetCurrentProcessId(),GetCurrentThreadId(),GetTickCount64(),id.instance,id.generation,id.sourceFrame,
                reinterpret_cast<uintptr_t>(InDeviceContext),jx,jy,gx==NVSDK_NGX_Result_Success,gy==NVSDK_NGX_Result_Success,
                mx,my,gmx==NVSDK_NGX_Result_Success,gmy==NVSDK_NGX_Result_Success,reset,gr==NVSDK_NGX_Result_Success,
                width,height,outWidth,outHeight,gw==NVSDK_NGX_Result_Success,gh==NVSDK_NGX_Result_Success,
                gow==NVSDK_NGX_Result_Success,goh==NVSDK_NGX_Result_Success,
                subrectWidth,subrectHeight,gsw==NVSDK_NGX_Result_Success,gsh==NVSDK_NGX_Result_Success,
                RenderWidth(),RenderHeight(),TargetWidth(),TargetHeight(),DisplayWidth(),DisplayHeight());
        }
        GenshinSubstage::ActualProviderCall providerCall;
        nvResult = NVNGXProxy::D3D11_EvaluateFeature()(InDeviceContext, _p_dlssHandle, InParameters, NULL);
        providerCall.Returned(static_cast<uint32_t>(nvResult), nvResult == NVSDK_NGX_Result_Success);

        if (GenshinSubstage::current.Known())
        {
            static std::atomic<uint32_t> observedReturns{0};
            if (observedReturns.fetch_add(1, std::memory_order_relaxed) < 6)
                LOG_INFO("Authenticated Genshin native D3D11 NGX return={:X} owner={} context={} serial={}",
                         static_cast<uint32_t>(nvResult), GenshinSubstage::current.ownerEpoch,
                         GenshinSubstage::current.context, GenshinSubstage::current.serial);
        }

        if (nvResult != NVSDK_NGX_Result_Success)
        {
            LOG_ERROR("_EvaluateFeature result: {0:X}", (unsigned int) nvResult);
            return false;
        }

        LOG_TRACE("_EvaluateFeature ok!");
    }
    else
    {
        GenshinSubstage::Event(GenshinSubstage::Stage::ProviderAttempt, GenshinSubstage::State::Failed,
            GenshinSubstage::Resource::Unknown, GenshinSubstage::Domain::Ngx, 0, GenshinSubstage::Issue::NoProvider);
        LOG_ERROR("_EvaluateFeature is nullptr");
        return false;
    }

    return true;
}

DLSSFeatureDx11::DLSSFeatureDx11(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters)
    : IFeature(InHandleId, InParameters), IFeature_Dx11(InHandleId, InParameters), DLSSFeature(InHandleId, InParameters)
{
    if (NVNGXProxy::NVNGXModule() == nullptr)
    {
        LOG_INFO("nvngx.dll not loaded, now loading");
        NVNGXProxy::InitNVNGX();
    }

    LOG_INFO("binding complete!");
}

DLSSFeatureDx11::~DLSSFeatureDx11()
{
    if (State::Instance().isShuttingDown)
        return;

    if (NVNGXProxy::D3D11_ReleaseFeature() != nullptr && _p_dlssHandle != nullptr)
        NVNGXProxy::D3D11_ReleaseFeature()(_p_dlssHandle);
}
