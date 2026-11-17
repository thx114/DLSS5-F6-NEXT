#include <pch.h>
#include <native/OpticalFgNrPolicy.h>
#include "FeatureProvider_Dx11.h"
#include <upscalers/FeatureLifecycleTrace.h>
#include <inputs/taa/TaaSourceOutput.h>
#include <proxies/NVNGX_Proxy.h>

#include "Util.h"
#include "Config.h"

#include "NVNGX_Parameter.h"
#include "FeatureTransition.h"

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

namespace {
thread_local std::optional<Upscaler> initializingDx11Backend;
struct BackendBinding {
    const IFeature_Dx11* feature=nullptr;
    Microsoft::WRL::ComPtr<IUnknown> device;
    std::optional<Upscaler> backend;
};
std::mutex backendBindingMutex;
BackendBinding backendBinding;
}
std::optional<Upscaler> FeatureProvider_Dx11::ActiveBackend(ID3D11Device* sourceDevice)
{
    // Init-local provider policy can run before a successful binding. A source
    // transport only sees the committed backend for its own real DX11 device.
    if(!sourceDevice && initializingDx11Backend)return initializingDx11Backend;
    Microsoft::WRL::ComPtr<IUnknown> identity;
    if(sourceDevice && FAILED(sourceDevice->QueryInterface(IID_PPV_ARGS(&identity))))return {};
    std::lock_guard lock(backendBindingMutex);
    return !sourceDevice||identity.Get()==backendBinding.device.Get()?backendBinding.backend:std::nullopt;
}
void FeatureProvider_Dx11::BindActiveFeature(IFeature_Dx11* feature,ID3D11DeviceContext* context)
{
    if(!feature||!context||!feature->IsInited())return;
    {std::lock_guard lock(backendBindingMutex);if(backendBinding.feature==feature)return;}
    Microsoft::WRL::ComPtr<ID3D11Device> device;context->GetDevice(&device);
    BackendBinding selected;selected.feature=feature;
    if(!device||FAILED(device.As(&selected.device)))return;
    selected.backend=feature->GetUpscalerType();
    {std::lock_guard lock(backendBindingMutex);std::swap(backendBinding,selected);}
    LOG_INFO("DX11 provider binding: handle={} backend={} source_device={}",
        feature->Handle()?feature->Handle()->Id:0,UpscalerDisplayName(feature->GetUpscalerType()),static_cast<void*>(device.Get()));
    // Release the previous COM identity after leaving the publication lock.
}
void FeatureProvider_Dx11::ForgetFeature(const IFeature_Dx11* feature)
{
    BackendBinding previous;
    {std::lock_guard lock(backendBindingMutex);
        if(backendBinding.feature==feature)std::swap(backendBinding,previous);}
}
bool FeatureProvider_Dx11::InitializeFeature(IFeature_Dx11* feature,ID3D11Device* device,
    ID3D11DeviceContext* context,NVSDK_NGX_Parameter* parameters)
{
    if(!feature)return false;
    const auto previous=initializingDx11Backend;
    initializingDx11Backend=feature->GetUpscalerType();
    struct Restore {std::optional<Upscaler> previous;~Restore(){initializingDx11Backend=previous;}} restore{previous};
    const bool initialized=feature->Init(device,context,parameters);
    if(initialized)BindActiveFeature(feature,context);else ForgetFeature(feature);
    return initialized;
}

bool FeatureProvider_Dx11::GetFeature(Upscaler upscaler, UINT handleId, NVSDK_NGX_Parameter* parameters,
                                      std::unique_ptr<IFeature_Dx11>* feature, bool preserveDx12Fallback)
{
    State& state = State::Instance();
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

    // The feature reports the active provider. A fallback must never turn the
    // user's auto request into a persistent explicit backend selection.

    return loaded;
}

bool FeatureProvider_Dx11::ChangeFeature(ID3D11Device* device, ID3D11DeviceContext* devContext,
                                         UINT handleId, NVSDK_NGX_Parameter* parameters,
                                         ContextData<IFeature_Dx11>* contextData, bool preserveDx12Fallback)
{
    State& state = State::Instance();
    Config& cfg = *Config::Instance();

    // A pending menu selection is not an Apply. Snapshot the committed request
    // before Retire; a newer Apply can supersede a queued fallback, and will be
    // replayed after an already-started rebuild has finished initialization.
    if (contextData->changeBackendCounter == 0)
    {
        const auto revision = BackendSelection::Dx11Revision();
        if (revision != contextData->rebuildRequestRevision)
            contextData->rebuildBackend.reset();
        contextData->rebuildRequestRevision = revision;
    }
    if (!contextData->rebuildBackend)
    {
        if (contextData->feature && contextData->feature->GetUpscalerType() == Upscaler::DLSSD)
            contextData->rebuildBackend = Upscaler::DLSSD;
        else
            contextData->rebuildBackend = ResolveBackend(cfg.Dx11Upscaler,
                taa::source_output::current && taa::source_output::current->Valid() &&
                    taa::source_output::current->handle==handleId &&
                    taa::source_output::current->context==reinterpret_cast<uintptr_t>(devContext) &&
                    taa::source_output::current->parameters==reinterpret_cast<uintptr_t>(parameters),
                cfg.Dx12Upscaler, IdentifyGpu::getPrimaryGpu().dlssCapable, NVNGXProxy::IsDx11Inited());
    }
    auto& target = *contextData->rebuildBackend;
    const auto clearAppliedSelection = [&] {
        if (cfg.Dx11Upscaler.has_value() && state.newBackend == cfg.Dx11Upscaler.value())
            state.newBackend = Upscaler::Reset;
    };

    // Rebuild requests have no exposure-only provenance here. Preserve unknown or
    // mixed requests, including required size/device/backend rebuilds. Reject
    // unsupported exposure toggles at the menu before writing config instead.
    const auto phase = FeatureTransition::Advance(contextData->changeBackendCounter);
    if (phase == FeatureTransition::Phase::Invalid)
    {
        LOG_ERROR("Invalid upscaler rebuild phase: {}", contextData->changeBackendCounter);
        return false;
    }
    FeatureLifecycleTrace lifecycle(contextData->changeBackendCounter == 1 ? "DX11 ChangeFeature retire" :
        contextData->changeBackendCounter == 2 ? "DX11 ChangeFeature construct" : "DX11 ChangeFeature initialize",
        handleId);

    // first release everything
    if (phase == FeatureTransition::Phase::Retire)
    {
        if (!native::OpticalFgNrEnabled() && state.currentFG != nullptr && state.activeFgInput == FGInput::Upscaler)
        {
            state.fgChanged = true;
            state.clearCapturedHudlesses = true;
        }

        if (contextData->feature != nullptr)
        {
            LOG_INFO("changing backend to {0}", UpscalerDisplayName(target));

            const bool isPassthrough = target == Upscaler::DLSSD || target == Upscaler::DLSS ||
                                       target == Upscaler::DLSS_on12;
            auto retired = FeatureTransition::Prepare(*contextData, parameters, isPassthrough,
                []() -> ContextData<IFeature_Dx11>::ParameterOwner {
                    return { GetNGXParameters(API::DX11, false),
                        [](NVSDK_NGX_Parameter* p) { delete static_cast<NVNGX_Parameters*>(p); } };
                }, [](auto& table, auto& feature) { FeatureTransition::CopyRecipe(table, feature); });
            if (!retired)
            {
                LOG_ERROR("Cannot prepare upscaler replacement; current feature retained");
                contextData->changeBackendCounter = 0;
                contextData->rebuildBackend.reset();
                return false;
            }

            ForgetFeature(retired->feature.get()); // CPU selection ends here; GPU owners still retire normally.
            State::Instance().currentFeature = nullptr;
            // The feature destructor still owns its existing GPU drain contract.
            // Its exact initialization table survives alongside it.
            Util::DelayedDestroy(std::move(retired));
        }
        else
        {
            LOG_ERROR("can't find handle {0} in Dx11Contexts!", handleId);

            contextData->rebuildBackend.reset();
            state.changeBackend[handleId] = false;

            contextData->ClearCreateParams();

            contextData->changeBackendCounter = 0;

            return false;
        }

        return true;
    }

    if (phase == FeatureTransition::Phase::Construct)
    {
        LOG_INFO("Creating new {} upscaler", UpscalerDisplayName(target));

        contextData->feature.reset();

        if (!GetFeature(target, handleId, contextData->createParams, &contextData->feature,
                        preserveDx12Fallback))
        {
            LOG_ERROR("Upscaler can't created");
            contextData->feature.reset();
            contextData->ClearCreateParams();
            contextData->changeBackendCounter = 0;
            contextData->rebuildBackend.reset();
            state.changeBackend[handleId] = false;
            return false;
        }

        return true;
    }

    if (phase == FeatureTransition::Phase::Initialize)
    {
        // then init and continue
        auto initResult = InitializeFeature(contextData->feature.get(), device, devContext, contextData->createParams);

        if (cfg.Dx11DelayedInit.value_or_default())
        {
            FeatureLifecycleTrace delayed("DX11 ChangeFeature configured-delay", handleId);
            LOG_TRACE("sleeping after new Init of new feature for 1000ms");
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }

        contextData->changeBackendCounter = 0;

        if (BackendSelection::Dx11Revision() != contextData->rebuildRequestRevision)
        {
            // This completed instance still owns its original GPU lifetime.
            // Retire it through the normal next rebuild before applying the new request.
            contextData->rebuildBackend.reset();
            state.changeBackend[handleId] = true;
            return false;
        }

        if (!initResult || !contextData->feature->ModuleLoaded())
        {
            LOG_ERROR("init failed with {0} feature", UpscalerDisplayName(contextData->feature->GetUpscalerType()));

            if (target != Upscaler::DLSSD)
            {
                target = FallbackUpscaler(contextData->feature->GetUpscalerType(), preserveDx12Fallback);
                state.changeBackend[handleId] = true;
                ImGui::InsertNotification({ ImGuiToastType::Warning, 10000,
                    target == Upscaler::FSR22_on12 ? "Falling back to FSR 2.2 (DX12 bridge)" : "Falling back to FSR 2.2" });
            }
            else
            {
                clearAppliedSelection();
                contextData->rebuildBackend.reset();
                state.changeBackend[handleId] = false;
            }

            return false;
        }
        else
        {
            LOG_INFO("init successful for {0}, upscaler changed",
                     UpscalerDisplayName(contextData->feature->GetUpscalerType()));

            clearAppliedSelection();
            contextData->rebuildBackend.reset();
            state.changeBackend[handleId] = false;
        }

        contextData->ClearCreateParams();
    }

    // if initial feature can't be inited
    state.currentFeature = contextData->feature.get();

    return true;
}
