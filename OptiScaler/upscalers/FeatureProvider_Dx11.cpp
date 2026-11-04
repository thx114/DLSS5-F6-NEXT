#include <pch.h>
#include <native/OpticalFgNrPolicy.h>
#include "FeatureProvider_Dx11.h"
#include <upscalers/FeatureLifecycleTrace.h>

#include "Util.h"
#include "Config.h"

#include "NVNGX_Parameter.h"

#include "upscalers/dlss/DLSSFeature_Dx11.h"
#include "upscalers/dlss/DLSSFeature_Dx11On12.h"
#include "upscalers/dlssd/DLSSDFeature_Dx11.h"
#include "upscalers/fsr2/FSR2Feature_Dx11.h"
#include "upscalers/fsr2/FSR2Feature_Dx11On12.h"
#include "upscalers/fsr2_212/FSR2Feature_Dx11On12_212.h"
#include "upscalers/fsr31/FSR31Feature_Dx11.h"
#include "upscalers/ffx/FFXFeature_Dx11On12.h"
#include "upscalers/xess/XeSSFeature_Dx11.h"
#include "upscalers/xess/XeSSFeature_Dx11on12.h"
#include <misc/IdentifyGpu.h>
#include <imgui/ImGuiNotify.hpp>

bool FeatureProvider_Dx11::GetFeature(Upscaler upscaler, UINT handleId, NVSDK_NGX_Parameter* parameters,
                                      std::unique_ptr<IFeature_Dx11>* feature, bool preserveDx12Fallback)
{
    State& state = State::Instance();
    Config& cfg = *Config::Instance();
    auto primaryGpu = IdentifyGpu::getPrimaryGpu();
    const auto fallback = FallbackUpscaler(upscaler, preserveDx12Fallback);
    const auto makeFallback = [&]() {
        if (fallback == Upscaler::FSR22_on12)
            *feature = std::make_unique<FSR2FeatureDx11on12>(handleId, parameters);
        else
            *feature = std::make_unique<FSR2FeatureDx11>(handleId, parameters);
        upscaler = fallback;
    };

    switch (upscaler)
    {
    case Upscaler::XeSS:
        *feature = std::make_unique<XeSSFeature_Dx11>(handleId, parameters);
        break;

    case Upscaler::XeSS_on12:
        *feature = std::make_unique<XeSSFeatureDx11on12>(handleId, parameters);
        break;

    case Upscaler::FSR21_on12:
        *feature = std::make_unique<FSR2FeatureDx11on12_212>(handleId, parameters);
        break;

    case Upscaler::FSR22:
        *feature = std::make_unique<FSR2FeatureDx11>(handleId, parameters);
        break;

    case Upscaler::FSR22_on12:
        *feature = std::make_unique<FSR2FeatureDx11on12>(handleId, parameters);
        break;

    case Upscaler::FSR31:
        *feature = std::make_unique<FSR31FeatureDx11>(handleId, parameters);
        break;

    case Upscaler::FFX_on12:
        *feature = std::make_unique<FFXFeatureDx11on12>(handleId, parameters);
        break;

    case Upscaler::DLSS_on12:
        // DLSS across the bridge. The only way a D3D11 game can have both DLSS and Neural
        // Rendering, because the model will not initialise on a D3D11 device.
        if (primaryGpu.dlssCapable && state.NVNGX_DLSS_Path.has_value())
        {
            *feature = std::make_unique<DLSSFeatureDx11on12>(handleId, parameters);
            break;
        }

        // The caller decides whether this scope has an admitted bridge fallback.
        // Ordinary explicit on12 requests retain their DX12 consumer transport.
        makeFallback();
        break;

    case Upscaler::DLSS:
        if (primaryGpu.dlssCapable && state.NVNGX_DLSS_Path.has_value())
        {
            *feature = std::make_unique<DLSSFeatureDx11>(handleId, parameters);
            break;
        }
        else
        {
            *feature = std::make_unique<FSR2FeatureDx11>(handleId, parameters);
            upscaler = Upscaler::FSR22;
            break;
        }

    case Upscaler::DLSSD:
        if (primaryGpu.dlssCapable && state.NVNGX_DLSSD_Path.has_value())
        {
            *feature = std::make_unique<DLSSDFeatureDx11>(handleId, parameters);
            break;
        }
        else
        {
            *feature = std::make_unique<FSR2FeatureDx11>(handleId, parameters);
            upscaler = Upscaler::FSR22;
            break;
        }

    default:
        *feature = std::make_unique<FSR2FeatureDx11>(handleId, parameters);
        upscaler = Upscaler::FSR22;
        break;
    }

    bool loaded = (*feature)->ModuleLoaded();

    if (!loaded)
    {
        // A constructed fallback is not evidence that its module is available.
        ImGui::InsertNotification({ ImGuiToastType::Warning, 10000,
            fallback == Upscaler::FSR22_on12 ? "Falling back to FSR 2.2 (DX12 bridge)" : "Falling back to FSR 2.2" });
        makeFallback();
        loaded = (*feature)->ModuleLoaded();
        if (!loaded)
        {
            LOG_ERROR("Fallback {} module is unavailable", UpscalerDisplayName(upscaler));
            return false;
        }
    }

    // DLSSD is stored in the config as DLSS
    if (upscaler == Upscaler::DLSSD)
        upscaler = Upscaler::DLSS;

    cfg.Dx11Upscaler = upscaler;

    return loaded;
}

bool FeatureProvider_Dx11::ChangeFeature(Upscaler upscaler, ID3D11Device* device, ID3D11DeviceContext* devContext,
                                         UINT handleId, NVSDK_NGX_Parameter* parameters,
                                         ContextData<IFeature_Dx11>* contextData, bool preserveDx12Fallback)
{
    State& state = State::Instance();
    Config& cfg = *Config::Instance();

    const bool dlssOnNonCapable = !IdentifyGpu::getPrimaryGpu().dlssCapable && state.newBackend == Upscaler::DLSS;
    if (state.newBackend == Upscaler::Reset || dlssOnNonCapable)
        state.newBackend = cfg.Dx11Upscaler.value_or_default();

    // Rebuild requests have no exposure-only provenance here. Preserve unknown or
    // mixed requests, including required size/device/backend rebuilds. Reject
    // unsupported exposure toggles at the menu before writing config instead.
    contextData->changeBackendCounter++;
    FeatureLifecycleTrace lifecycle(contextData->changeBackendCounter == 1 ? "DX11 ChangeFeature retire" :
        contextData->changeBackendCounter == 2 ? "DX11 ChangeFeature construct" : "DX11 ChangeFeature initialize",
        handleId);

    // first release everything
    if (contextData->changeBackendCounter == 1)
    {
        if (!native::OpticalFgNrEnabled() && state.currentFG != nullptr && state.activeFgInput == FGInput::Upscaler)
        {
            state.fgChanged = true;
            state.clearCapturedHudlesses = true;
        }

        if (contextData->feature != nullptr)
        {
            LOG_INFO("changing backend to {0}", UpscalerDisplayName(state.newBackend));

            auto* dc = contextData->feature.get();
            // Use given params if using DLSS passthrough
            //
            // DLSS_on12 belongs here for the same reason the other two do: it is DLSS underneath, and
            // it wants the game's own parameter block rather than a synthetic one. Without it a
            // mid-session switch -- from the menu, on a resolution change, or through the auto-exposure
            // re-init -- rebuilt the feature from GetNGXParameters carrying only flags, resolutions and
            // quality, which quietly replaced the game's render presets with the default. That made
            // "pick dlss_12 in the menu" and "set dlss_12 in the ini" two different code paths, which
            // is a poor thing to discover while testing a fix.
            const bool isPassthrough = state.newBackend == Upscaler::DLSSD || state.newBackend == Upscaler::DLSS ||
                                       state.newBackend == Upscaler::DLSS_on12;

            // Retain a failed attempt's temporary table with its retiring feature.
            // In particular, never take ownership of the game's parameter table.
            auto retired = std::make_unique<ContextData<IFeature_Dx11>>();
            retired->feature = std::move(contextData->feature);
            retired->createParamsOwner = std::move(contextData->createParamsOwner);
            contextData->createParams = nullptr;
            if (isPassthrough)
                contextData->BorrowCreateParams(parameters);
            else
                contextData->OwnCreateParams({ GetNGXParameters(API::DX11, false),
                    [](NVSDK_NGX_Parameter* p) {
                        delete static_cast<NVNGX_Parameters*>(p);
                    } });
            contextData->createParams->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, dc->GetFeatureFlags());
            contextData->createParams->Set(NVSDK_NGX_Parameter_Width, dc->RenderWidth());
            contextData->createParams->Set(NVSDK_NGX_Parameter_Height, dc->RenderHeight());
            contextData->createParams->Set(NVSDK_NGX_Parameter_OutWidth, dc->DisplayWidth());
            contextData->createParams->Set(NVSDK_NGX_Parameter_OutHeight, dc->DisplayHeight());
            contextData->createParams->Set(NVSDK_NGX_Parameter_PerfQualityValue, dc->PerfQualityValue());

            State::Instance().currentFeature = nullptr;

            Util::DelayedDestroy(std::move(retired));

            // LOG_TRACE("sleeping before reset of current feature for 1000ms");
            // std::this_thread::sleep_for(std::chrono::milliseconds(1000));

            // contextData->feature.reset();
            // contextData->feature = nullptr;
        }
        else
        {
            LOG_ERROR("can't find handle {0} in Dx11Contexts!", handleId);

            state.newBackend = Upscaler::Reset;
            state.changeBackend[handleId] = false;

            contextData->ClearCreateParams();

            contextData->changeBackendCounter = 0;

            return false;
        }

        return true;
    }

    if (contextData->changeBackendCounter == 2)
    {
        LOG_INFO("Creating new {} upscaler", UpscalerDisplayName(state.newBackend));

        contextData->feature.reset();

        if (!GetFeature(state.newBackend, handleId, contextData->createParams, &contextData->feature,
                        preserveDx12Fallback))
        {
            LOG_ERROR("Upscaler can't created");
            contextData->feature.reset();
            contextData->ClearCreateParams();
            contextData->changeBackendCounter = 0;
            state.newBackend = Upscaler::Reset;
            state.changeBackend[handleId] = false;
            return false;
        }

        return true;
    }

    if (contextData->changeBackendCounter == 3)
    {
        // then init and continue
        auto initResult = contextData->feature->Init(device, devContext, contextData->createParams);

        if (cfg.Dx11DelayedInit.value_or_default())
        {
            FeatureLifecycleTrace delayed("DX11 ChangeFeature configured-delay", handleId);
            LOG_TRACE("sleeping after new Init of new feature for 1000ms");
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }

        contextData->changeBackendCounter = 0;

        if (!initResult || !contextData->feature->ModuleLoaded())
        {
            LOG_ERROR("init failed with {0} feature", UpscalerDisplayName(contextData->feature->GetUpscalerType()));

            if (state.newBackend != Upscaler::DLSSD)
            {
                state.newBackend = FallbackUpscaler(contextData->feature->GetUpscalerType(), preserveDx12Fallback);
                state.changeBackend[handleId] = true;
                ImGui::InsertNotification({ ImGuiToastType::Warning, 10000,
                    state.newBackend == Upscaler::FSR22_on12 ? "Falling back to FSR 2.2 (DX12 bridge)" : "Falling back to FSR 2.2" });
            }
            else
            {
                state.newBackend = Upscaler::Reset;
                state.changeBackend[handleId] = false;
            }

            return false;
        }
        else
        {
            LOG_INFO("init successful for {0}, upscaler changed",
                     UpscalerDisplayName(contextData->feature->GetUpscalerType()));

            state.newBackend = Upscaler::Reset;
            state.changeBackend[handleId] = false;
        }

        contextData->ClearCreateParams();
    }

    // if initial feature can't be inited
    state.currentFeature = contextData->feature.get();

    return true;
}
