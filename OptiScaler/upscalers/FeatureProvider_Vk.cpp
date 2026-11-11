#include <pch.h>
#include "FeatureProvider_Vk.h"

#include "Util.h"
#include "Config.h"

#include "NVNGX_Parameter.h"
#include "FeatureTransition.h"

#include "upscalers/fsr2/FSR2Feature_Vk.h"
#include "upscalers/dlss/DLSSFeature_Vk.h"
#include "upscalers/dlssd/DLSSDFeature_Vk.h"
#include "upscalers/fsr2_212/FSR2Feature_Vk_212.h"
#include "upscalers/fsr2_212/FSR2Feature_VkOnDx12_212.h"
#include "upscalers/ffx/FFXFeature_Vk.h"
#include "upscalers/xess/XeSSFeature_Vk.h"
#include "upscalers/ffx/FFXFeature_VkOn12.h"
#include "upscalers/dlss/DLSSFeature_VkOn12.h"
#include <misc/IdentifyGpu.h>

bool FeatureProvider_Vk::IsOn12(Upscaler upscaler)
{
    return upscaler == Upscaler::DLSS_on12 || upscaler == Upscaler::FFX_on12 || upscaler == Upscaler::FSR21_on12;
}

Upscaler FeatureProvider_Vk::On12For(Upscaler upscaler)
{
    if (IsOn12(upscaler))
        return upscaler;

    switch (upscaler)
    {
    case Upscaler::DLSS:
    case Upscaler::DLSSD:
        if (IdentifyGpu::getPrimaryGpu().dlssCapable && State::Instance().NVNGX_DLSS_Path.has_value())
            return Upscaler::DLSS_on12;

        return Upscaler::FFX_on12;

    case Upscaler::FSR21:
        return Upscaler::FSR21_on12;

    default:
        return Upscaler::FFX_on12;
    }
}

Upscaler FeatureProvider_Vk::ForFrameGenerationOnly(Upscaler upscaler)
{
    if (!Config::Instance()->DlssNrNativeFrameGenerationOnly.value_or_default())
        return upscaler;

    return On12For(upscaler);
}

bool FeatureProvider_Vk::GetFeature(Upscaler upscaler, UINT handleId, NVSDK_NGX_Parameter* parameters,
                                    std::unique_ptr<IFeature_Vk>* feature)
{
    State& state = State::Instance();
    Config& cfg = *Config::Instance();
    auto primaryGpu = IdentifyGpu::getPrimaryGpu();

    switch (upscaler)
    {
    case Upscaler::XeSS:
        *feature = std::make_unique<XeSSFeature_Vk>(handleId, parameters);
        break;

    case Upscaler::FSR21:
        *feature = std::make_unique<FSR2FeatureVk212>(handleId, parameters);
        break;

    case Upscaler::FSR21_on12:
        *feature = std::make_unique<FSR2FeatureVkOnDx12_212>(handleId, parameters);
        break;

    case Upscaler::FSR22:
        *feature = std::make_unique<FSR2FeatureVk>(handleId, parameters);
        break;

    case Upscaler::FFX:
        *feature = std::make_unique<FFXFeatureVk>(handleId, parameters);
        break;

    case Upscaler::DLSS_on12:
        *feature = std::make_unique<DLSSFeatureVkOn12>(handleId, parameters);
        break;

    case Upscaler::FFX_on12:
        *feature = std::make_unique<FFXFeatureVkOn12>(handleId, parameters);
        break;

    case Upscaler::DLSS:
        if (primaryGpu.dlssCapable && state.NVNGX_DLSS_Path.has_value())
        {
            *feature = std::make_unique<DLSSFeatureVk>(handleId, parameters);
            break;
        }
        else
        {
            *feature = std::make_unique<FSR2FeatureVk>(handleId, parameters);
            upscaler = Upscaler::FSR22;
            break;
        }

    case Upscaler::DLSSD:
        if (primaryGpu.dlssCapable && state.NVNGX_DLSSD_Path.has_value())
        {
            *feature = std::make_unique<DLSSDFeatureVk>(handleId, parameters);
            break;
        }
        else
        {
            *feature = std::make_unique<FSR2FeatureVk>(handleId, parameters);
            upscaler = Upscaler::FSR22;
            break;
        }

    default:
        *feature = std::make_unique<FSR2FeatureVk>(handleId, parameters);
        upscaler = Upscaler::FSR22;
        break;
    }

    bool loaded = (*feature)->ModuleLoaded();

    if (!loaded)
    {
        // Fail after the constructor
        ImGui::InsertNotification({ ImGuiToastType::Warning, 10000, "Falling back to FSR 2.2" });
        *feature = std::make_unique<FSR2FeatureVk>(handleId, parameters);
        upscaler = Upscaler::FSR22;
        loaded = (*feature)->ModuleLoaded(); // Construction alone does not establish availability.
    }

    // DLSSD is stored in the config as DLSS
    if (upscaler == Upscaler::DLSSD)
        upscaler = Upscaler::DLSS;

    cfg.VulkanUpscaler = upscaler;

    return loaded;
}

bool FeatureProvider_Vk::ChangeFeature(Upscaler upscaler, VkInstance instance, VkPhysicalDevice pd, VkDevice device,
                                       VkCommandBuffer cmdBuffer, PFN_vkGetInstanceProcAddr gipa,
                                       PFN_vkGetDeviceProcAddr gdpa, UINT handleId, NVSDK_NGX_Parameter* parameters,
                                       ContextData<IFeature_Vk>* contextData)
{
    State& state = State::Instance();
    Config& cfg = *Config::Instance();

    const bool dlssOnNonCapable = !IdentifyGpu::getPrimaryGpu().dlssCapable && state.newBackend == Upscaler::DLSS;
    if (state.newBackend == Upscaler::Reset || dlssOnNonCapable)
        state.newBackend = cfg.VulkanUpscaler.value_or_default();

    const auto phase = FeatureTransition::Advance(contextData->changeBackendCounter);
    if (phase == FeatureTransition::Phase::Invalid)
    {
        LOG_ERROR("Invalid upscaler rebuild phase: {}", contextData->changeBackendCounter);
        return false;
    }

    LOG_INFO("changeBackend is true, counter: {0}", contextData->changeBackendCounter);

    // first release everything
    if (phase == FeatureTransition::Phase::Retire)
    {
        if (contextData->feature != nullptr)
        {
            LOG_INFO("changing backend to {0}", UpscalerDisplayName(state.newBackend));

            const bool isPassthrough = state.newBackend == Upscaler::DLSSD || state.newBackend == Upscaler::DLSS;
            auto retired = FeatureTransition::Prepare(*contextData, parameters, isPassthrough,
                []() -> ContextData<IFeature_Vk>::ParameterOwner {
                    return { GetNGXParameters(API::Vulkan, false),
                        [](NVSDK_NGX_Parameter* p) { delete static_cast<NVNGX_Parameters*>(p); } };
                }, [](auto& table, auto& feature) { FeatureTransition::CopyRecipe(table, feature); });
            if (!retired)
            {
                LOG_ERROR("Cannot prepare upscaler replacement; current feature retained");
                contextData->changeBackendCounter = 0;
                return false;
            }

            vkDeviceWaitIdle(device);

            State::Instance().currentFeature = nullptr;
            // The feature destructor still owns its existing GPU drain contract.
            // Its exact initialization table survives alongside it.
            Util::DelayedDestroy(std::move(retired));
        }
        else
        {
            LOG_ERROR("can't find handle {0} in VkContexts!", handleId);

            state.newBackend = Upscaler::Reset;
            state.changeBackend[handleId] = false;

            contextData->ClearCreateParams();

            contextData->changeBackendCounter = 0;

            return false;
        }

        return true;
    }

    if (phase == FeatureTransition::Phase::Construct)
    {
        LOG_INFO("Creating new {} upscaler", UpscalerDisplayName(state.newBackend));

        contextData->feature.reset();

        if (!GetFeature(state.newBackend, handleId, contextData->createParams, &contextData->feature))
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

    if (phase == FeatureTransition::Phase::Initialize)
    {
        // next frame create context
        auto initResult = false;
        {
            ScopedSkipSpoofingGlobal skipSpoofingGlobal {};
            initResult =
                contextData->feature->Init(instance, pd, device, cmdBuffer, gipa, gdpa, contextData->createParams);
        }

        contextData->changeBackendCounter = 0;

        if (!initResult || !contextData->feature->ModuleLoaded())
        {
            LOG_ERROR("init failed with {0} feature", UpscalerDisplayName(state.newBackend));

            if (state.newBackend != Upscaler::DLSSD)
            {
                if (cfg.VulkanUpscaler == Upscaler::DLSS)
                {
                    state.newBackend = Upscaler::XeSS;
                    ImGui::InsertNotification({ ImGuiToastType::Warning, 10000, "Falling back to XeSS" });
                }
                else
                {
                    state.newBackend = Upscaler::FSR21;
                    ImGui::InsertNotification({ ImGuiToastType::Warning, 10000, "Falling back to FSR 2.1.2" });
                }
            }
            else
            {
                // Retry DLSSD
                state.newBackend = Upscaler::DLSSD;
            }

            state.changeBackend[handleId] = true;

            return false;
        }
        else
        {
            LOG_INFO("init successful for {0}, upscaler changed", UpscalerDisplayName(state.newBackend));

            state.newBackend = Upscaler::Reset;
            state.changeBackend[handleId] = false;
        }

        contextData->ClearCreateParams();
    }

    // if initial feature can't be inited
    state.currentFeature = contextData->feature.get();

    return true;
}
