#include "pch.h"

#include "Config.h"
#include <runtime/ModuleLifetime.h>
#include "Util.h"

#include <upscalers/IFeature_Dx11.h>
#include <upscalers/IFeature_Dx11wDx12.h>
#include <inputs/taa/TaaSourceOutput.h>
#include <upscalers/FeatureLifecycleTrace.h>
#include <upscalers/FeatureProvider_Dx11.h>
#include <upscalers/FeatureTransition.h>

#include "NVNGX_DLSS.h"
#include "NVNGX_Parameter.h"
#include "proxies/NVNGX_Proxy.h"

#include <with_dx12/with_dx12.h>
#include "FG/Upscaler_Inputs_Dx11wDx12.h"

#include <ankerl/unordered_dense.h>
#include <imgui/ImGuiNotify.hpp>
#include <misc/IdentifyGpu.h>
#include <runtime/GuideDiagnostics.h>
#include "F6EvalReceiptV1.h"
#include "BeforeSrInput.h"
#include "NeoBeforeSrInput.h"
#include <integration/HoYoShade.h>
#include <intrin.h>

static ID3D11Device* D3D11Device = nullptr;
static ID3D11Device* taaAttachedDevice = nullptr; // nonowning; the TAA session retains the actual device
static ankerl::unordered_dense::map<unsigned int, ContextData<IFeature_Dx11>> Dx11Contexts;
static int evalCounter = 0;
static bool shutdown = false;
static bool _skipInit = false;
static wchar_t const** paths;

class ScopedInitDx11
{
  private:
    bool previousState;

  public:
    ScopedInitDx11()
    {
        previousState = _skipInit;
        _skipInit = true;
    }

    ~ScopedInitDx11() { _skipInit = previousState; }
};

static void UpdateInitPaths(NVSDK_NGX_FeatureCommonInfo* InFeatureInfo)
{
    State::Instance().NVNGX_FeatureInfo_Paths.clear();

    if (InFeatureInfo != nullptr)
    {
        auto exePath = Util::ExePath().remove_filename();

        std::optional<std::filesystem::path> nvngxDlssPath = std::nullopt;
        std::optional<std::filesystem::path> nvngxDlssDPath = std::nullopt;
        std::optional<std::filesystem::path> nvngxDlssGPath = std::nullopt;

        // Check DLSS path
        if (State::Instance().NVNGX_DLSS_Path.has_value())
        {
            nvngxDlssPath = std::filesystem::path(State::Instance().NVNGX_DLSS_Path.value());
        }
        else
        {
            auto path = Util::FindFilePath(exePath, "nvngx_dlss.dll");

            if (path.has_value())
                nvngxDlssPath = path.value();
        }

        // Check DLSS-D path
        if (State::Instance().NVNGX_DLSSD_Path.has_value())
        {
            nvngxDlssDPath = std::filesystem::path(State::Instance().NVNGX_DLSSD_Path.value());
        }
        else
        {
            auto path = Util::FindFilePath(exePath, "nvngx_dlssd.dll");

            if (path.has_value())
                nvngxDlssDPath = path.value();
        }

        // Check DLSS-G path
        if (State::Instance().NVNGX_DLSSG_Path.has_value())
        {
            nvngxDlssGPath = std::filesystem::path(State::Instance().NVNGX_DLSSG_Path.value());
        }
        else
        {
            auto path = Util::FindFilePath(exePath, "nvngx_dlssg.dll");

            if (path.has_value())
                nvngxDlssGPath = path.value();
        }

        // Override locations
        if (Config::Instance()->DLSSFeaturePath.has_value())
            State::Instance().NVNGX_FeatureInfo_Paths.push_back(Config::Instance()->DLSSFeaturePath.value());

        // If DLSS path is overriden
        if (Config::Instance()->NVNGX_DLSS_Library.has_value() && nvngxDlssPath.has_value())
            State::Instance().NVNGX_FeatureInfo_Paths.push_back(nvngxDlssPath.value().parent_path().wstring());

        // OptiDll Path
        State::Instance().NVNGX_FeatureInfo_Paths.push_back(Config::Instance()->MainDllPath.value());

        // Original paths from NVNGX
        for (size_t i = 0; i < InFeatureInfo->PathListInfo.Length; i++)
        {
            const wchar_t* path = InFeatureInfo->PathListInfo.Path[i];
            State::Instance().NVNGX_FeatureInfo_Paths.push_back(std::wstring(path));
        }

        // Exe path
        State::Instance().NVNGX_FeatureInfo_Paths.push_back(exePath.wstring());

        // If DLSS path is not overriden
        if (!Config::Instance()->NVNGX_DLSS_Library.has_value() && nvngxDlssPath.has_value())
            State::Instance().NVNGX_FeatureInfo_Paths.push_back(nvngxDlssPath.value().parent_path().wstring());

        // Add found locations
        if (nvngxDlssDPath.has_value())
            State::Instance().NVNGX_FeatureInfo_Paths.push_back(nvngxDlssDPath.value().parent_path().wstring());

        if (nvngxDlssGPath.has_value())
            State::Instance().NVNGX_FeatureInfo_Paths.push_back(nvngxDlssGPath.value().parent_path().wstring());

        // Build pointer array
        paths = new const wchar_t*[State::Instance().NVNGX_FeatureInfo_Paths.size()];
        for (size_t i = 0; i < State::Instance().NVNGX_FeatureInfo_Paths.size(); ++i)
        {
            paths[i] = State::Instance().NVNGX_FeatureInfo_Paths[i].c_str();
            LOG_DEBUG("Feature Path [{}]: {}", i, wstring_to_string(State::Instance().NVNGX_FeatureInfo_Paths[i]));
        }

        InFeatureInfo->PathListInfo.Path = paths;
        InFeatureInfo->PathListInfo.Length = (int) State::Instance().NVNGX_FeatureInfo_Paths.size();
    }
}

#pragma region NVSDK_NGX_D3D11_Init

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_Init_Ext(unsigned long long InApplicationId,
                                                        const wchar_t* InApplicationDataPath, ID3D11Device* InDevice,
                                                        NVSDK_NGX_Version InSDKVersion,
                                                        const NVSDK_NGX_FeatureCommonInfo* InFeatureInfo)
{
    // A real game Init takes ownership of its global input binding.
    taaAttachedDevice=nullptr;
    NVSDK_NGX_FeatureCommonInfo localFeatureInfo = {};

    if (InFeatureInfo != nullptr)
        std::memcpy(&localFeatureInfo, InFeatureInfo, sizeof(NVSDK_NGX_FeatureCommonInfo));

    if (!_skipInit)
        UpdateInitPaths(&localFeatureInfo);

    State::Instance().NVNGX_ApplicationId = InApplicationId;
    State::Instance().NVNGX_ApplicationDataPath = std::wstring(InApplicationDataPath);
    State::Instance().NVNGX_Version = InSDKVersion;
    State::Instance().NVNGX_FeatureInfo = InFeatureInfo;
    State::Instance().NVNGX_Version = InSDKVersion;

    if (Config::Instance()->DLSSEnabled.value_or_default() && !_skipInit)
    {
        if (Config::Instance()->UseGenericAppIdWithDlss.value_or_default())
            InApplicationId = app_id_override;

        if (NVNGXProxy::NVNGXModule() == nullptr)
            NVNGXProxy::InitNVNGX();

        if (NVNGXProxy::NVNGXModule() != nullptr && NVNGXProxy::D3D11_Init_Ext() != nullptr)
        {
            LOG_INFO("calling NVNGXProxy::D3D11_Init_Ext");

            auto result = NVNGXProxy::D3D11_Init_Ext()(InApplicationId, InApplicationDataPath, InDevice, InSDKVersion,
                                                       &localFeatureInfo);

            LOG_INFO("calling NVNGXProxy::D3D11_Init_Ext result: {0:X}", (UINT) result);

            if (result == NVSDK_NGX_Result_Success)
                NVNGXProxy::SetDx11Inited(true);
        }
        else
        {
            LOG_WARN("NVNGXProxy::NVNGXModule or NVNGXProxy::D3D11_Init_Ext is nullptr!");
        }
    }

    if (InFeatureInfo != nullptr && InSDKVersion > 0x0000013)
        State::Instance().NVNGX_Logger = InFeatureInfo->LoggingInfo;

    if (State::Instance().nvngxDx11Inited && InDevice == D3D11Device)
    {
        LOG_WARN("NVNGX already inited");
        return NVSDK_NGX_Result_Success;
    }

    LOG_INFO("AppId: {0}", InApplicationId);
    LOG_INFO("SDK: {0:x}", (unsigned int) InSDKVersion);
    LOG_INFO(L"InApplicationDataPath {0}", std::wstring(InApplicationDataPath));

    if (InDevice)
        D3D11Device = InDevice;

    State::Instance().currentD3D11Device = InDevice;
    State::Instance().nvngxDx11Inited = true;

    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_Init(unsigned long long InApplicationId,
                                                    const wchar_t* InApplicationDataPath, ID3D11Device* InDevice,
                                                    const NVSDK_NGX_FeatureCommonInfo* InFeatureInfo,
                                                    NVSDK_NGX_Version InSDKVersion)
{
    NVSDK_NGX_FeatureCommonInfo localFeatureInfo = {};

    if (InFeatureInfo != nullptr)
        std::memcpy(&localFeatureInfo, InFeatureInfo, sizeof(NVSDK_NGX_FeatureCommonInfo));

    if (!_skipInit)
        UpdateInitPaths(&localFeatureInfo);

    if (Config::Instance()->DLSSEnabled.value_or_default() && !_skipInit)
    {
        if (Config::Instance()->UseGenericAppIdWithDlss.value_or_default())
            InApplicationId = app_id_override;

        if (NVNGXProxy::NVNGXModule() == nullptr)
            NVNGXProxy::InitNVNGX();

        if (NVNGXProxy::NVNGXModule() != nullptr && NVNGXProxy::D3D11_Init() != nullptr)
        {
            LOG_INFO("calling NVNGXProxy::D3D11_Init");

            auto result = NVNGXProxy::D3D11_Init()(InApplicationId, InApplicationDataPath, InDevice, &localFeatureInfo,
                                                   InSDKVersion);

            LOG_INFO("calling NVNGXProxy::D3D11_Init result: {0:X}", (UINT) result);

            if (result == NVSDK_NGX_Result_Success)
                NVNGXProxy::SetDx11Inited(true);
        }
    }

    ScopedInitDx11 scopedInit {};
    auto result = NVSDK_NGX_D3D11_Init_Ext(0x1337, InApplicationDataPath, InDevice, InSDKVersion, &localFeatureInfo);
    LOG_DEBUG("was called NVSDK_NGX_D3D11_Init_Ext");
    return result;
}

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_Init_ProjectID(const char* InProjectId,
                                                              NVSDK_NGX_EngineType InEngineType,
                                                              const char* InEngineVersion,
                                                              const wchar_t* InApplicationDataPath,
                                                              ID3D11Device* InDevice, NVSDK_NGX_Version InSDKVersion,
                                                              const NVSDK_NGX_FeatureCommonInfo* InFeatureInfo)
{
    NVSDK_NGX_FeatureCommonInfo localFeatureInfo = {};

    if (InFeatureInfo != nullptr)
        std::memcpy(&localFeatureInfo, InFeatureInfo, sizeof(NVSDK_NGX_FeatureCommonInfo));

    if (!_skipInit)
        UpdateInitPaths(&localFeatureInfo);

    if (Config::Instance()->DLSSEnabled.value_or_default() && !_skipInit)
    {
        if (Config::Instance()->UseGenericAppIdWithDlss.value_or_default())
            InProjectId = project_id_override;

        if (NVNGXProxy::NVNGXModule() == nullptr)
            NVNGXProxy::InitNVNGX();

        if (NVNGXProxy::NVNGXModule() != nullptr && NVNGXProxy::D3D11_Init_ProjectID() != nullptr)
        {
            LOG_INFO("calling NVNGXProxy::D3D11_Init_ProjectID");

            auto result =
                NVNGXProxy::D3D11_Init_ProjectID()(InProjectId, InEngineType, InEngineVersion, InApplicationDataPath,
                                                   InDevice, InSDKVersion, &localFeatureInfo);

            LOG_INFO("calling NVNGXProxy::D3D11_Init_ProjectID result: {0:X}", (UINT) result);

            if (result == NVSDK_NGX_Result_Success)
                NVNGXProxy::SetDx11Inited(true);
        }
    }

    ScopedInitDx11 scopedInit {};
    auto result = NVSDK_NGX_D3D11_Init_Ext(0x1337, InApplicationDataPath, InDevice, InSDKVersion, &localFeatureInfo);

    LOG_INFO("InProjectId: {0}", InProjectId);
    LOG_INFO("InEngineType: {0}", (int) InEngineType);
    LOG_INFO("InEngineVersion: {0}", InEngineVersion);

    State::Instance().NVNGX_ProjectId = std::string(InProjectId);
    State::Instance().NVNGX_Engine = InEngineType;
    State::Instance().NVNGX_EngineVersion = std::string(InEngineVersion);

    return result;
}

// Not sure about this one, original nvngx does not export this method
NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_Init_with_ProjectID(
    const char* InProjectId, NVSDK_NGX_EngineType InEngineType, const char* InEngineVersion,
    const wchar_t* InApplicationDataPath, ID3D11Device* InDevice, const NVSDK_NGX_FeatureCommonInfo* InFeatureInfo,
    NVSDK_NGX_Version InSDKVersion)
{
    auto result = NVSDK_NGX_D3D11_Init_Ext(0x1337, InApplicationDataPath, InDevice, InSDKVersion, InFeatureInfo);

    LOG_INFO("InProjectId: {0}", InProjectId);
    LOG_INFO("InEngineType: {0}", (int) InEngineType);
    LOG_INFO("InEngineVersion: {0}", InEngineVersion);

    State::Instance().NVNGX_ProjectId = std::string(InProjectId);
    State::Instance().NVNGX_Engine = InEngineType;
    State::Instance().NVNGX_EngineVersion = std::string(InEngineVersion);

    return result;
}

#pragma endregion

#pragma region NVSDK_NGX_D3D11_Shutdown

struct ShutdownDx11Request
{
    ID3D11Device* device = nullptr;
    bool deviceSpecific = false;
};
static thread_local ShutdownDx11Request shutdownDx11Request;

static NVSDK_NGX_Result ShutdownDx11Body(ID3D11Device* device = nullptr, bool shutdown1 = false)
{
    // External feature caches can suppress ReleaseFeature and forget their
    // parked handles at Shutdown. Retire our actual owners directly; calling
    // the exported ReleaseFeature here could be intercepted by the same ASI.
    for (auto it = Dx11Contexts.begin(); it != Dx11Contexts.end();)
    {
        auto* feature = it->second.feature.get();
        if (feature && !feature->PrepareForShutdown())
        {
            LOG_ERROR("DX11 Shutdown: feature {} could not retire; owner and runtime retained", it->first);
            return NVSDK_NGX_Result_Fail;
        }
        if (State::Instance().currentFeature == feature)
            State::Instance().currentFeature = nullptr;
        State::Instance().changeBackend.erase(it->first);
        it = Dx11Contexts.erase(it);
    }

    shutdown = true;
    D3D11Device = nullptr;
    taaAttachedDevice = nullptr;
    State::Instance().currentFeature = nullptr;

    if (Config::Instance()->DLSSEnabled.value_or_default() && NVNGXProxy::IsDx11Inited())
    {
        auto stop = NVNGXProxy::D3D11_Shutdown();
        auto stopDevice = shutdown1 ? NVNGXProxy::D3D11_Shutdown1() : nullptr;
        if (stopDevice || stop)
        {
            const auto result = stopDevice ? stopDevice(device) : stop();
            if (result != NVSDK_NGX_Result_Success)
            {
                shutdown = false;
                return result;
            }
            NVNGXProxy::SetDx11Inited(false);
        }
    }

    // Unhooking and cleaning stuff causing issues during shutdown.
    // Disabled for now to check if it cause any issues
    // HooksDx::UnHook();

    shutdown = false;
    State::Instance().nvngxDx11Inited = false;

    Dx11WithDx12::ResetUpscalerResourceCache(true);

    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_Shutdown()
{
    if (!ModuleLifetime::CleanupAllowed()) return NVSDK_NGX_Result_Fail;
    return ShutdownDx11Body(shutdownDx11Request.device, shutdownDx11Request.deviceSpecific);
}

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_Shutdown1(ID3D11Device* InDevice)
{
    if (!ModuleLifetime::CleanupAllowed()) return NVSDK_NGX_Result_Fail;
    struct RestoreRequest
    {
        ShutdownDx11Request previous;
        ~RestoreRequest() { shutdownDx11Request = previous; }
    } restore { std::exchange(shutdownDx11Request, ShutdownDx11Request { InDevice, true }) };
    // Legacy NGX observers (including external feature caches) hook Shutdown
    // rather than Shutdown1. Traverse that entry so they discard old handles,
    // while preserving the device-specific vendor call in our common body.
    // Volatile keeps this as a real call through the detour even under LTCG.
    auto* volatile shutdownEntry = &NVSDK_NGX_D3D11_Shutdown;
    return shutdownEntry();
}

#pragma endregion

#pragma region NVSDK_NGX_D3D11 Parameters

/**
 * @brief [Deprecated NGX API] Superceeded by NVSDK_NGX_AllocateParameters and NVSDK_NGX_GetCapabilityParameters.
 *
 * Retrieves a common NVSDK parameter map for providing params to the SDK. The lifetime of this
 * map is NOT managed by the application. It is expected to be managed internally by the SDK.
 */
NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_GetParameters(NVSDK_NGX_Parameter** OutParameters)
{
    LOG_FUNC();

    if (OutParameters == nullptr)
        return NVSDK_NGX_Result_FAIL_InvalidParameter;

    if (Config::Instance()->DLSSEnabled.value_or_default() && NVNGXProxy::NVNGXModule() != nullptr &&
        NVNGXProxy::D3D11_GetParameters() != nullptr)
    {
        LOG_INFO("calling NVNGXProxy::D3D11_GetParameters");

        auto result = NVNGXProxy::D3D11_GetParameters()(OutParameters);

        LOG_INFO("calling NVNGXProxy::D3D11_GetParameters result: {0:X}", (UINT) result);

        if (result == NVSDK_NGX_Result_Success)
        {
            InitNGXParameters(*OutParameters, API::DX11);
            SetNGXParamAllocType(*(*OutParameters), NGX_AllocTypes::NVPersistent);
            return result;
        }
    }

    // Get custom parameters if using custom backend
    static NVNGX_Parameters oldParams = NVNGX_Parameters(API::DX11, true);
    *OutParameters = &oldParams;
    InitNGXParameters(*OutParameters, API::DX11);

    LOG_DEBUG("Returning custom Opti parameters");

    return NVSDK_NGX_Result_Success;
}

/**
 * @brief Allocates a new NVSDK parameter map pre-populated with NGX capabilities and information about available
 * features. The output parameter map may also be used in the same ways as a parameter map allocated with
 * AllocateParameters(). The lifetime of this map is managed by the calling application with DestroyParameters().
 */
NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_GetCapabilityParameters(NVSDK_NGX_Parameter** OutParameters)
{
    LOG_FUNC();

    if (OutParameters == nullptr)
        return NVSDK_NGX_Result_FAIL_InvalidParameter;

    if (Config::Instance()->DLSSEnabled.value_or_default() && NVNGXProxy::NVNGXModule() != nullptr &&
        NVNGXProxy::IsDx11Inited() && NVNGXProxy::D3D11_GetCapabilityParameters() != nullptr)
    {
        LOG_INFO("calling NVNGXProxy::D3D11_GetCapabilityParameters");

        auto result = NVNGXProxy::D3D11_GetCapabilityParameters()(OutParameters);

        LOG_INFO("calling NVNGXProxy::D3D11_GetCapabilityParameters result: {0:X}", (UINT) result);

        if (result == NVSDK_NGX_Result_Success)
        {
            InitNGXParameters(*OutParameters, API::DX11);
            SetNGXParamAllocType(*(*OutParameters), NGX_AllocTypes::NVDynamic);
            return result;
        }
    }

    *OutParameters = new NVNGX_Parameters(API::DX11, false);
    InitNGXParameters(*OutParameters, API::DX11);

    LOG_DEBUG("Returning custom Opti parameters");

    return NVSDK_NGX_Result_Success;
}

/**
 * @brief Allocates a new parameter map used to provide parameters needed by the DLSS API. The lifetime of this map
 * is managed by the calling application with DestroyParameters().
 */
NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_AllocateParameters(NVSDK_NGX_Parameter** OutParameters)
{
    LOG_FUNC();

    if (OutParameters == nullptr) return NVSDK_NGX_Result_FAIL_InvalidParameter;

    if (Config::Instance()->DLSSEnabled.value_or_default() && NVNGXProxy::NVNGXModule() != nullptr &&
        NVNGXProxy::D3D11_AllocateParameters() != nullptr)
    {
        LOG_INFO("calling NVNGXProxy::D3D11_AllocateParameters");

        auto result = NVNGXProxy::D3D11_AllocateParameters()(OutParameters);

        LOG_INFO("calling NVNGXProxy::D3D11_AllocateParameters result: {0:X}", (UINT) result);

        if (result == NVSDK_NGX_Result_Success)
        {
            SetNGXParamAllocType(*(*OutParameters), NGX_AllocTypes::NVDynamic);
            return result;
        }
    }

    *OutParameters = new NVNGX_Parameters(API::DX11, false);

    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_PopulateParameters_Impl(NVSDK_NGX_Parameter* InParameters)
{
    LOG_FUNC();

    if (InParameters == nullptr)
        return NVSDK_NGX_Result_FAIL_InvalidParameter;

    InitNGXParameters(InParameters, API::DX11);

    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_DestroyParameters(NVSDK_NGX_Parameter* InParameters)
{
    if (!ModuleLifetime::CleanupAllowed()) return NVSDK_NGX_Result_Fail;
    LOG_FUNC();

    if (InParameters == nullptr)
        return NVSDK_NGX_Result_Fail;

    const bool success = TryDestroyNGXParameters(InParameters, NVNGXProxy::D3D11_DestroyParameters());

    return success ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_Fail;
}

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_GetScratchBufferSize(NVSDK_NGX_Feature InFeatureId,
                                                                    const NVSDK_NGX_Parameter* InParameters,
                                                                    size_t* OutSizeInBytes)
{
    LOG_WARN("-> 52428800");
    *OutSizeInBytes = 52428800;
    return NVSDK_NGX_Result_Success;
}

#pragma endregion

#pragma region NVSDK_NGX_D3D11 Feature

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_CreateFeature(ID3D11DeviceContext* InDevCtx,
                                                             NVSDK_NGX_Feature InFeatureID,
                                                             NVSDK_NGX_Parameter* InParameters,
                                                             NVSDK_NGX_Handle** OutHandle)
{
    if (!InDevCtx || !InParameters || !OutHandle) return NVSDK_NGX_Result_Fail;
    FeatureLifecycleTrace lifecycle("DX11 CreateFeature");
    // FeatureId check
    if (InFeatureID != NVSDK_NGX_Feature_SuperSampling && InFeatureID != NVSDK_NGX_Feature_RayReconstruction)
    {
        if (Config::Instance()->DLSSEnabled.value_or_default() && NVNGXProxy::InitDx11(D3D11Device) &&
            NVNGXProxy::D3D11_CreateFeature() != nullptr)
        {
            auto result = NVNGXProxy::D3D11_CreateFeature()(InDevCtx, InFeatureID, InParameters, OutHandle);
            LOG_INFO("D3D11_CreateFeature result for ({0}): {1:X}", (int) InFeatureID, (UINT) result);
            return result;
        }
        else
        {
            LOG_ERROR("Can't create this feature ({0})!", (int) InFeatureID);
            return NVSDK_NGX_Result_Fail;
        }
    }

    // CreateFeature
    auto handleId = IFeature::GetNextHandleId();
    if (!handleId) return NVSDK_NGX_Result_Fail;
    LOG_INFO("HandleId: {0}", handleId);

    if (InFeatureID == NVSDK_NGX_Feature_SuperSampling)
    {
        const bool taaInput = taa::source_output::Creating(reinterpret_cast<uintptr_t>(InDevCtx),reinterpret_cast<uintptr_t>(InParameters));
        const auto upscalerChoice = FeatureProvider_Dx11::ResolveBackend(Config::Instance()->Dx11Upscaler,
            taaInput, Config::Instance()->Dx12Upscaler, IdentifyGpu::getPrimaryGpu().dlssCapable, NVNGXProxy::IsDx11Inited());
        LOG_INFO("DX11 backend request: explicit={} taa={} dx12_preference={} resolved={}; configuration retained",
            Config::Instance()->Dx11Upscaler.has_value(), taaInput,
            UpscalerDisplayName(Config::Instance()->Dx12Upscaler.value_or_default()), UpscalerDisplayName(upscalerChoice));
        // Provider GPU/library gates and the current-output receipt remain authoritative.
        LOG_INFO("Creating new {} feature", UpscalerDisplayName(upscalerChoice));

        Dx11Contexts[handleId] = {};

        // All providers must return the exact current output receipt.
        if (!FeatureProvider_Dx11::GetFeature(upscalerChoice, handleId, InParameters, &Dx11Contexts[handleId].feature,
                                             true))
        {
            LOG_ERROR("Can't create {} feature", UpscalerDisplayName(upscalerChoice));
            return NVSDK_NGX_Result_Fail;
        }
    }
    else if (InFeatureID == NVSDK_NGX_Feature_RayReconstruction)
    {
        LOG_INFO("Creating new DLSSD feature");

        Dx11Contexts[handleId] = {};

        if (!FeatureProvider_Dx11::GetFeature(Upscaler::DLSSD, handleId, InParameters, &Dx11Contexts[handleId].feature,
                                             true))
        {
            LOG_ERROR("Can't create DLSSD feature");
            return NVSDK_NGX_Result_Fail;
        }
    }

    // Register every successfully created handle immediately. A later menu Apply
    // must reach handles that have not issued their first Evaluate yet.
    State::Instance().changeBackend[handleId] = false;
    State::Instance().api = DX11;
    auto deviceContext = Dx11Contexts[handleId].feature.get();
    *OutHandle = deviceContext->Handle();

    // Always get device from context to avoid issues with Dx11 w/Dx12
    LOG_DEBUG("Get Dx11Device from InDevCtx!");
    InDevCtx->GetDevice(&D3D11Device);
    evalCounter = 0;

    if (!D3D11Device)
    {
        LOG_ERROR("Can't get Dx11Device from InDevCtx!");
        return NVSDK_NGX_Result_Fail;
    }

    D3D11Device->Release();

    State::Instance().autoExposure.reset();

    if (deviceContext->ModuleLoaded() && FeatureProvider_Dx11::InitializeFeature(deviceContext,D3D11Device,InDevCtx,InParameters))
    {
        State::Instance().currentFeature = deviceContext;

        return NVSDK_NGX_Result_Success;
    }


    LOG_ERROR("CreateFeature failed");

    Dx11Contexts[handleId].rebuildBackend = FeatureProvider_Dx11::FallbackUpscaler(
        deviceContext->GetUpscalerType(), true);
    Dx11Contexts[handleId].rebuildRequestRevision = BackendSelection::Dx11Revision();
    State::Instance().changeBackend[handleId] = true;

    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_ReleaseFeature(NVSDK_NGX_Handle* InHandle)
{
    if (!ModuleLifetime::CleanupAllowed()) return NVSDK_NGX_Result_Fail;
    if (!InHandle)
        return NVSDK_NGX_Result_Success;

    auto handleId = InHandle->Id;
    FeatureLifecycleTrace lifecycle("DX11 ReleaseFeature", handleId);
    if (handleId < DLSS_MOD_ID_OFFSET)
    {
        if (Config::Instance()->DLSSEnabled.value_or_default() && NVNGXProxy::D3D11_ReleaseFeature() != nullptr)
        {
            auto result = NVNGXProxy::D3D11_ReleaseFeature()(InHandle);

            if (!shutdown)
                LOG_INFO("D3D11_ReleaseFeature result for ({0}): {1:X}", handleId, (UINT) result);

            return result;
        }
        else
        {
            return NVSDK_NGX_Result_FAIL_FeatureNotFound;
        }
    }

    if (!shutdown)
        LOG_INFO("releasing feature with id {0}", handleId);

    auto existingContext = Dx11Contexts.find(handleId);
    if (existingContext == Dx11Contexts.end())
        return NVSDK_NGX_Result_Success;

    if (auto deviceContext = existingContext->second.feature.get(); deviceContext != nullptr)
    {
        // Release uses the same owner retirement contract as Shutdown. A fixed
        // delay cannot prove GPU completion and stalls every bridge recreation.
        if (!deviceContext->PrepareForShutdown())
        {
            LOG_ERROR("DX11 Release: feature {} could not retire; owner and runtime retained", handleId);
            return NVSDK_NGX_Result_Fail;
        }

        if (deviceContext == State::Instance().currentFeature)
            State::Instance().currentFeature = nullptr;

        existingContext->second.feature.reset();
        if (!shutdown &&
            Config::Instance()->Dx11DelayedInit.value_or_default())
        {
            LOG_TRACE("sleeping for 500ms after reset()!");
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

    // A release between ChangeFeature phases can have no feature but still own
    // a temporary table. Erase that context too; borrowed tables are untouched.
    if (auto it = Dx11Contexts.find(handleId); it != Dx11Contexts.end())
        Dx11Contexts.erase(it);
    State::Instance().changeBackend.erase(handleId);

    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_GetFeatureRequirements(
    IDXGIAdapter* Adapter, const NVSDK_NGX_FeatureDiscoveryInfo* FeatureDiscoveryInfo,
    NVSDK_NGX_FeatureRequirement* OutSupported)
{
    LOG_FUNC();

    if (FeatureDiscoveryInfo->FeatureID == NVSDK_NGX_Feature_SuperSampling)
    {
        if (OutSupported == nullptr)
            OutSupported = new NVSDK_NGX_FeatureRequirement();

        OutSupported->FeatureSupported = NVSDK_NGX_FeatureSupportResult_Supported;
        OutSupported->MinHWArchitecture = 0;

        // Some windows 10 os version
        strcpy_s(OutSupported->MinOSVersion, "10.0.10240.16384");
        return NVSDK_NGX_Result_Success;
    }

    if (Config::Instance()->DLSSEnabled.value_or_default() && NVNGXProxy::NVNGXModule() == nullptr)
        NVNGXProxy::InitNVNGX();

    if (Config::Instance()->DLSSEnabled.value_or_default() && NVNGXProxy::D3D11_GetFeatureRequirements() != nullptr)
    {
        LOG_DEBUG("D3D11_GetFeatureRequirements for ({0})", (int) FeatureDiscoveryInfo->FeatureID);
        auto result = NVNGXProxy::D3D11_GetFeatureRequirements()(Adapter, FeatureDiscoveryInfo, OutSupported);
        LOG_DEBUG("result for D3D11_GetFeatureRequirements ({0}): {1:X}", (int) FeatureDiscoveryInfo->FeatureID,
                  (UINT) result);
        return result;
    }

    OutSupported->FeatureSupported = NVSDK_NGX_FeatureSupportResult_AdapterUnsupported;
    return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
}

namespace NgxInput {
bool AttachTaaDx11Input(ID3D11Device* device) {
    if (!device || shutdown || State::Instance().isShuttingDown || (D3D11Device && D3D11Device!=device)) return false;
    if (!D3D11Device) taaAttachedDevice=device;
    D3D11Device=device;
    State::Instance().currentD3D11Device=device;
    State::Instance().api=API::DX11;
    return true;
}
bool HasTaaDx11Context(unsigned int id) {
    const auto it=Dx11Contexts.find(id);
    return it!=Dx11Contexts.end() && FeatureTransition::HasLiveContext(it->second);
}
bool ReleaseTaaDx11Feature(unsigned int id) {
    if (!ModuleLifetime::CleanupAllowed()) return false;
    const auto it=Dx11Contexts.find(id);
    if (it==Dx11Contexts.end()) return true; // already released by NGX shutdown
    auto* feature=it->second.feature.get();
    if (feature && !feature->PrepareForShutdown()) return false;
    if (State::Instance().currentFeature==feature) State::Instance().currentFeature=nullptr;
    Dx11Contexts.erase(it);
    State::Instance().changeBackend.erase(id);
    return true;
}
void DetachTaaDx11Input(ID3D11Device* device) {
    if (taaAttachedDevice!=device || !Dx11Contexts.empty()) return;
    if (State::Instance().currentD3D11Device==device) State::Instance().currentD3D11Device=nullptr;
    if (D3D11Device==device) D3D11Device=nullptr;
    taaAttachedDevice=nullptr;
}
}

static NVSDK_NGX_Result EvaluateDx11(ID3D11DeviceContext* InDevCtx,
                                    const NVSDK_NGX_Handle* InFeatureHandle,
                                    NVSDK_NGX_Parameter* InParameters,
                                    PFN_NVSDK_NGX_ProgressCallback InCallback,
                                    neo::ngx::F6EvalReceiptV1* receipt,
                                    const interop::BeforeSrInput* beforeSr = nullptr)
{
    const auto noOutput = [receipt](NVSDK_NGX_Result result) {
        if (receipt) receipt->status = neo::ngx::F6OutputStatus::no_output;
        return result;
    };
    if (InFeatureHandle == nullptr)
    {

        LOG_DEBUG("InFeatureHandle is null");
        return noOutput(NVSDK_NGX_Result_FAIL_FeatureNotFound);
    }
    else
    {
        LOG_DEBUG("NVSDK_NGX_D3D11_EvaluateFeature Handle: {0}", InFeatureHandle->Id);
    }

    if (InDevCtx == nullptr)
    {

        LOG_ERROR("InDevCtx is null!!!");
        return noOutput(NVSDK_NGX_Result_Fail);
    }

    if (!InParameters)
        return noOutput(NVSDK_NGX_Result_FAIL_InvalidParameter);

    State& state = State::Instance();
    auto handleId = InFeatureHandle->Id;
    if (handleId < DLSS_MOD_ID_OFFSET)
    {
        if (Config::Instance()->DLSSEnabled.value_or_default() && NVNGXProxy::D3D11_EvaluateFeature() != nullptr)
        {
            LOG_DEBUG("D3D11_EvaluateFeature for ({0})", handleId);
            auto result = NVNGXProxy::D3D11_EvaluateFeature()(InDevCtx, InFeatureHandle, InParameters, InCallback);
            LOG_INFO("D3D11_EvaluateFeature result for ({0}): {1:X}", handleId, (UINT) result);
            return result;
        }
        else
        {
            return NVSDK_NGX_Result_FAIL_FeatureNotFound;
        }
    }

    GuideDiagnostics::EvaluationScope evaluateObservation(FrameTrace::Api::D3D11, handleId,
        reinterpret_cast<uint64_t>(InDevCtx));
    evalCounter++;
    if (Config::Instance()->SkipFirstFrames.has_value() && evalCounter < Config::Instance()->SkipFirstFrames.value())
    {

        evaluateObservation.Result(NVSDK_NGX_Result_Success);
        return noOutput(NVSDK_NGX_Result_Success);
    }

    if (InCallback)
        LOG_INFO("callback exist");

    IFeature_Dx11* deviceContext = nullptr;
    auto activeContext = &Dx11Contexts[handleId];

    if (state.changeBackend[handleId])
    {
        auto successfulPhase = FeatureProvider_Dx11::ChangeFeature(D3D11Device, InDevCtx, handleId,
                                                                   InParameters, activeContext,
                                                                   true);

        evalCounter = 0;

        if (activeContext->changeBackendCounter != 0 || !successfulPhase)
        {
            evaluateObservation.Result(NVSDK_NGX_Result_Success);
            return noOutput(NVSDK_NGX_Result_Success);
        }
    }

    if (activeContext->feature == nullptr) // prevent source api name flicker when dlssg is active
    {
        state.setInputApiName = state.currentInputApiName;
    }
    else
    {
        deviceContext = activeContext->feature.get();
        FeatureProvider_Dx11::BindActiveFeature(deviceContext,InDevCtx);
        state.currentFeature = deviceContext;
    }

    const auto targetApiName =
        !state.setInputApiName.has_value() ? ApiUpscalerInput::DLSS_DX11 : state.setInputApiName.value();

    if (state.currentInputApiName != targetApiName)
        state.currentInputApiName = targetApiName;

    state.setInputApiName.reset();

    if (deviceContext == nullptr)
    {

        LOG_DEBUG("trying to use released handle, returning NVSDK_NGX_Result_Success");
        evaluateObservation.Result(NVSDK_NGX_Result_Success);
        return noOutput(NVSDK_NGX_Result_Success);
    }

    // Bind the active producer capability to this actual backend and parameter
    // block before translation. Native DX11 retains its existing NR route.
    if (taa::source_output::current) {
        const auto resource=[&](const char* key) {ID3D11Resource* r=nullptr;InParameters->Get(key,&r);return reinterpret_cast<uintptr_t>(r);};
        const auto* call=taa::source_output::current;
        taa::source_output::SelectBackend({reinterpret_cast<uintptr_t>(InDevCtx),reinterpret_cast<uintptr_t>(InParameters),
            resource(NVSDK_NGX_Parameter_Color),resource(NVSDK_NGX_Parameter_Depth),
            resource(NVSDK_NGX_Parameter_MotionVectors),resource(NVSDK_NGX_Parameter_Output),handleId,call->request},
            dynamic_cast<IFeature_Dx11wDx12*>(deviceContext)!=nullptr);
    }
    ID3D11Resource* orderedOutput = nullptr;
    const auto upscaleResult = deviceContext->EvaluateWithOutput(InDevCtx, InParameters, &orderedOutput, beforeSr);
    if (receipt)
    {
        receipt->provider_frame = static_cast<uint64_t>(deviceContext->FrameCount());
        if (upscaleResult && orderedOutput &&
            reinterpret_cast<uintptr_t>(orderedOutput) == receipt->output)
            receipt->status = neo::ngx::F6OutputStatus::current_output_ordered;
    }


    const auto upscaler = deviceContext->GetUpscalerType();
    const auto fallback = FeatureProvider_Dx11::FallbackUpscaler(upscaler,
        true);
    const bool bridgeFallback = fallback == Upscaler::FSR22_on12;

    // A failed ordinary bridge evaluation must not publish its cached guides as fresh.
    if (upscaleResult &&
        State::Instance().activeFgInput == FGInput::Upscaler)
    {
        if (WithDx12::IsInited())
        {
            auto cq = WithDx12::GetD3D12CommandQueue();
            auto device = WithDx12::GetD3D12Device();

            UpscalerInputsDx11wDx12::Init(D3D11Device, InDevCtx, device, cq);

            UpscalerInputsDx11wDx12::UpscaleStart(InParameters, deviceContext);
            UpscalerInputsDx11wDx12::UpscaleEnd(InParameters, deviceContext);
        }
    }

    if (!upscaleResult && !deviceContext->IsInited() &&
        (bridgeFallback || upscaler == Upscaler::XeSS || upscaler == Upscaler::XeSS_on12 ||
         upscaler == Upscaler::DLSS || upscaler == Upscaler::FFX_on12))
    {
        ImGui::InsertNotification({ ImGuiToastType::Error, 10000, "Upscaler failed to run!" });
        activeContext->rebuildBackend = fallback;
        activeContext->rebuildRequestRevision = BackendSelection::Dx11Revision();
        state.changeBackend[handleId] = true;
    }

    evaluateObservation.Result(NVSDK_NGX_Result_Success);
    return NVSDK_NGX_Result_Success;
}

// Ordinary NGX provides exact current resources. Typed inputs can be filtered
// as stored source values without a bridge or an invented transfer declaration.
static interop::BeforeSrInput DescribeNgxInput(ID3D11DeviceContext* context,
    const NVSDK_NGX_Handle* handle, NVSDK_NGX_Parameter* parameters)
{
    interop::BeforeSrInput input;
    if (!context || !handle || !parameters || !HoYoShade::Selected(HoYoShade::Stage::BeforeSr)) return input;
    input.origin=interop::SrInputOrigin::Ngx; input.query=interop::BeforeSrQuery::Invalid;
    const auto texture=[&](const char* key,Microsoft::WRL::ComPtr<ID3D11Texture2D>& out) {
        ID3D11Resource* resource=nullptr;
        if(parameters->Get(key,&resource)!=NVSDK_NGX_Result_Success)
            parameters->Get(key,reinterpret_cast<void**>(&resource));
        return resource && SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&out)));
    };
    Microsoft::WRL::ComPtr<ID3D11Texture2D> color,depth,motion,output;
    if(context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE || !texture(NVSDK_NGX_Parameter_Color,color) ||
        !texture(NVSDK_NGX_Parameter_Depth,depth) || !texture(NVSDK_NGX_Parameter_MotionVectors,motion) ||
        !texture(NVSDK_NGX_Parameter_Output,output)) return input;
    D3D11_TEXTURE2D_DESC c{},o{};color->GetDesc(&c);output->GetDesc(&o);
    if(c.MipLevels!=1 || c.ArraySize!=1 || c.SampleDesc.Count!=1) return input;
    input.query=interop::BeforeSrQuery::Matched;input.parameters=parameters;input.feature=handle->Id;
    static std::atomic<uint64_t> requests{0};input.request=requests.fetch_add(1,std::memory_order_relaxed)+1;
    auto& f=input.frame;f.context=context;f.color=color.Get();f.depth=depth.Get();f.motion=motion.Get();
    f.frame=input.request;f.instance=handle->Id;f.render_width=c.Width;f.render_height=c.Height;
    parameters->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,&f.render_width);
    parameters->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,&f.render_height);
    f.display_width=o.Width;f.display_height=o.Height;f.color_storage_format=f.color_view_format=c.Format;
    f.effect_domain=interop::EffectDomain::SourceValues;
    f.source_color=color.Get();f.source_storage_format=f.source_view_format=c.Format;
    return input;
}

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_EvaluateFeature(ID3D11DeviceContext* context,
    const NVSDK_NGX_Handle* handle, NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback callback)
{
    const auto input=DescribeNgxInput(context,handle,parameters);
    return EvaluateDx11(context, handle, parameters, callback, nullptr, &input);
}

namespace NgxInput {
NVSDK_NGX_Result EvaluateDx11Input(ID3D11DeviceContext* context, const NVSDK_NGX_Handle* handle,
    NVSDK_NGX_Parameter* parameters, uint64_t requestId, neo::ngx::F6EvalReceiptV1* receipt,
    const interop::BeforeSrInput& input)
{
    if (!receipt || receipt->size != sizeof(*receipt) || receipt->version != 1)
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    *receipt = {};
    receipt->request_id = requestId;
    receipt->handle = reinterpret_cast<uintptr_t>(handle);
    receipt->context = reinterpret_cast<uintptr_t>(context);
    ID3D11Resource* output = nullptr;
    if (parameters && parameters->Get(NVSDK_NGX_Parameter_Output, &output) != NVSDK_NGX_Result_Success)
        parameters->Get(NVSDK_NGX_Parameter_Output, reinterpret_cast<void**>(&output));
    receipt->output = reinterpret_cast<uintptr_t>(output);
    auto current = input;
    if (current.HasDeclaration() && (!handle || !current.MatchesCall(context, parameters, handle->Id, requestId)))
        current.query = interop::BeforeSrQuery::Invalid;
    return EvaluateDx11(context, handle, parameters, nullptr, receipt, &current);
}
} // namespace NgxInput

extern "C" __declspec(dllexport) __declspec(noinline) NVSDK_NGX_Result NVSDK_CONV F6_NGX_D3D11_EvaluateFeatureV1(
    ID3D11DeviceContext* context, const NVSDK_NGX_Handle* handle, NVSDK_NGX_Parameter* parameters,
    PFN_NVSDK_NGX_ProgressCallback callback, uint64_t requestId, neo::ngx::F6EvalReceiptV1* receipt)
{
    if (!receipt || receipt->size != sizeof(*receipt) || receipt->version != 1)
        return NVSDK_NGX_Result_FAIL_InvalidParameter;

    *receipt = {};
    receipt->request_id = requestId;
    receipt->handle = reinterpret_cast<uintptr_t>(handle);
    receipt->context = reinterpret_cast<uintptr_t>(context);
    ID3D11Resource* output = nullptr;
    if (parameters && parameters->Get(NVSDK_NGX_Parameter_Output, &output) != NVSDK_NGX_Result_Success)
        parameters->Get(NVSDK_NGX_Parameter_Output, reinterpret_cast<void**>(&output));
    receipt->output = reinterpret_cast<uintptr_t>(output);

    interop::BeforeSrInput beforeSr;
    const auto* config = Config::Instance();
    const auto stage = static_cast<unsigned>(config->HoYoShadeStage.value_or_default());
    if (context && parameters && stage == static_cast<unsigned>(HoYoShade::Stage::BeforeSr) &&
        HoYoShade::ResolveEnabled(config->HoYoShadeEnabled.ReadObserved(), stage)) {
        const auto readResource = [parameters](const char* name) {
            ID3D11Resource* resource = nullptr;
            if (parameters->Get(name, &resource) != NVSDK_NGX_Result_Success)
                parameters->Get(name, reinterpret_cast<void**>(&resource));
            return resource;
        };
        beforeSr = interop::QueryBeforeSrInput(_ReturnAddress(), context,
            readResource(NVSDK_NGX_Parameter_Color), readResource(NVSDK_NGX_Parameter_Depth),
            readResource(NVSDK_NGX_Parameter_MotionVectors));
    }
    const auto result = EvaluateDx11(context, handle, parameters, callback, receipt, &beforeSr);
    LOG_DEBUG("F6 evaluate receipt v1 request={} handle=0x{:x} context=0x{:x} output=0x{:x} provider_frame={} status={} result={:x}",
        requestId, receipt->handle, receipt->context, receipt->output, receipt->provider_frame,
        static_cast<unsigned>(receipt->status), static_cast<unsigned>(result));
    return result;
}

#pragma endregion
