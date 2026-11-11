#include "pch.h"

#include "Config.h"
#include "Util.h"

#include <upscalers/IFeature_Dx11.h>
#include <upscalers/FeatureLifecycleTrace.h>
#include <upscalers/FeatureProvider_Dx11.h>

#include "NVNGX_DLSS.h"
#include "NVNGX_Parameter.h"
#include "proxies/NVNGX_Proxy.h"

#include <with_dx12/with_dx12.h>
#include "FG/Upscaler_Inputs_Dx11wDx12.h"

#include <ankerl/unordered_dense.h>
#include <imgui/ImGuiNotify.hpp>
#include <misc/IdentifyGpu.h>
#include <upscalers/PersistentSrDx11.h>
#include <genshin/GenshinSrReceipt.h>
#include <runtime/GuideDiagnostics.h>
#include "FSR2_Dx11_Internal.h"

static ID3D11Device* D3D11Device = nullptr;
static ankerl::unordered_dense::map<unsigned int, ContextData<IFeature_Dx11>> Dx11Contexts;
static int evalCounter = 0;
static bool shutdown = false;
static bool _skipInit = false;
static wchar_t const** paths;

// Only the authenticated internal Genshin call publishes this witness.
static void PublishGenshinSrOutcome(ID3D11DeviceContext* context, NVSDK_NGX_Parameter* parameters,
                                   const NVSDK_NGX_Handle* handle, GenshinSrReceipt::Outcome outcome,
                                   bool evaluated=false)
{
    if (!GenshinSrReceipt::active) return;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<IUnknown> outputIdentity;
    auto* output=DlssNr::NgxParameter::ReadResource<ID3D11Resource>(parameters,NVSDK_NGX_Parameter_Output);
    if (context) context->GetDevice(&device);
    if (output) output->QueryInterface(IID_PPV_ARGS(&outputIdentity));
    GenshinSrReceipt::Publish({reinterpret_cast<uint64_t>(device.Get()),reinterpret_cast<uint64_t>(context),
        reinterpret_cast<uint64_t>(parameters),reinterpret_cast<uint64_t>(outputIdentity.Get()),
        reinterpret_cast<uint64_t>(handle)},outcome,evaluated);
}

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

static NVSDK_NGX_Result ShutdownDx11Body()
{
    shutdown = true;

    // for (auto const& [key, val] : Dx11Contexts)
    //{
    //     if (val.feature)
    //         NVSDK_NGX_D3D11_ReleaseFeature(val.feature->Handle());
    // }

    // Dx11Contexts.clear();

    D3D11Device = nullptr;
    State::Instance().currentFeature = nullptr;

    if (Config::Instance()->DLSSEnabled.value_or_default() && NVNGXProxy::IsDx11Inited() &&
        NVNGXProxy::D3D11_Shutdown() != nullptr)
    {
        auto result = NVNGXProxy::D3D11_Shutdown()();
        NVNGXProxy::SetDx11Inited(false);
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
    PersistentSrDx11::Entry poolEntry(PersistentSrDx11::Instance());
    if (!poolEntry) return NVSDK_NGX_Result_Fail;
    PersistentSrDx11::Shutdown();
    return ShutdownDx11Body();
}

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_Shutdown1(ID3D11Device* InDevice)
{
    PersistentSrDx11::Entry poolEntry(PersistentSrDx11::Instance());
    if (!poolEntry) return NVSDK_NGX_Result_Fail;
    PersistentSrDx11::Shutdown();
    shutdown = true;

    if (Config::Instance()->DLSSEnabled.value_or_default() && NVNGXProxy::IsDx11Inited() &&
        NVNGXProxy::D3D11_Shutdown1() != nullptr)
    {
        auto result = NVNGXProxy::D3D11_Shutdown1()(InDevice);
        NVNGXProxy::SetDx11Inited(false);
    }

    return ShutdownDx11Body();
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

    if (OutParameters == nullptr) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    if (PersistentSrDx11::UseOwnedParameters())
    {
        static NVNGX_Parameters owned(API::DX11, true);
        *OutParameters = &owned;
        InitNGXParameters(*OutParameters, API::DX11);
        LOG_INFO("PersistentSR parameters source=owned api=GetParameters allocation=InternPersistent");
        return NVSDK_NGX_Result_Success;
    }

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

    if (OutParameters == nullptr) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    if (PersistentSrDx11::UseOwnedParameters())
    {
        *OutParameters = GetNGXParameters(API::DX11, false);
        LOG_INFO("PersistentSR parameters source=owned api=GetCapabilityParameters allocation=InternDynamic");
        return NVSDK_NGX_Result_Success;
    }

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
    if (PersistentSrDx11::UseOwnedParameters())
    {
        *OutParameters = new NVNGX_Parameters(API::DX11, false);
        LOG_INFO("PersistentSR parameters source=owned api=AllocateParameters allocation=InternDynamic");
        return NVSDK_NGX_Result_Success;
    }

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
    if (GenshinSrReceipt::active && (!InDevCtx || !InParameters || !OutHandle ||
        InFeatureID != NVSDK_NGX_Feature_SuperSampling))
    {
        PublishGenshinSrOutcome(InDevCtx,InParameters,nullptr,GenshinSrReceipt::Outcome::UnsupportedFeature);
        return NVSDK_NGX_Result_Fail;
    }
    PersistentSrDx11::Entry poolEntry(PersistentSrDx11::Instance());
    if (!poolEntry) return NVSDK_NGX_Result_Fail;
    if (!InDevCtx || !InParameters || !OutHandle) return NVSDK_NGX_Result_Fail;
    if (auto pooled = PersistentSrDx11::TryCreate(InDevCtx, InFeatureID, InParameters, OutHandle))
    {
        if (*pooled == NVSDK_NGX_Result_Success)
        {
            InDevCtx->GetDevice(&D3D11Device);
            if (D3D11Device) D3D11Device->Release();
        }
        return *pooled;
    }
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
        Upscaler upscalerChoice = Upscaler::FSR22; // Default FSR 2.2.1

        // If original NVNGX available use DLSS as base upscaler
        if (IdentifyGpu::getPrimaryGpu().dlssCapable && NVNGXProxy::IsDx11Inited())
            upscalerChoice = Upscaler::DLSS;

        if (Config::Instance()->Dx11Upscaler.has_value())
            upscalerChoice = Config::Instance()->Dx11Upscaler.value();

        // The authenticated internal Genshin FSR dispatch must use the D3D11-on-D3D12
        // DLSS path so its private D3D11 resources can reach the native DLSS provider.
        // Keep the existing GPU/provider gates in FeatureProvider_Dx11; without them
        // this request still fails closed or falls back to the configured FSR path.
        if (GenshinSrReceipt::active)
        {
            upscalerChoice = Upscaler::DLSS_on12;
            LOG_INFO("Authenticated Genshin SR dispatch selecting DLSS_on12 bridge");
        }

        LOG_INFO("Creating new {} feature", UpscalerDisplayName(upscalerChoice));

        Dx11Contexts[handleId] = {};

        // The authenticated producer still requires its DLSS-specific receipt.
        if (!FeatureProvider_Dx11::GetFeature(upscalerChoice, handleId, InParameters, &Dx11Contexts[handleId].feature,
                                             !GenshinSrReceipt::active))
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
                                             !GenshinSrReceipt::active))
        {
            LOG_ERROR("Can't create DLSSD feature");
            return NVSDK_NGX_Result_Fail;
        }
    }

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

    if (deviceContext->ModuleLoaded() && deviceContext->Init(D3D11Device, InDevCtx, InParameters))
    {
        State::Instance().currentFeature = deviceContext;
        PublishGenshinSrOutcome(InDevCtx,InParameters,*OutHandle,GenshinSrReceipt::Outcome::AwaitingEvaluate);
        return NVSDK_NGX_Result_Success;
    }

    PublishGenshinSrOutcome(InDevCtx,InParameters,*OutHandle,GenshinSrReceipt::Outcome::CreateRebuild);
    LOG_ERROR("CreateFeature failed");

    State::Instance().newBackend = FeatureProvider_Dx11::FallbackUpscaler(
        deviceContext->GetUpscalerType(), !GenshinSrReceipt::active);
    State::Instance().changeBackend[handleId] = true;

    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_ReleaseFeature(NVSDK_NGX_Handle* InHandle)
{
    PersistentSrDx11::Entry poolEntry(PersistentSrDx11::Instance());
    if (!poolEntry) return NVSDK_NGX_Result_Fail;
    if (auto pooled = PersistentSrDx11::TryRelease(InHandle)) return *pooled;
    if (!InHandle)
        return NVSDK_NGX_Result_Success;

    auto handleId = InHandle->Id;
    if (PersistentSrDx11::Instance().OwnsId(handleId)) return NVSDK_NGX_Result_FAIL_FeatureNotFound;
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
        if (!shutdown && !GenshinFsrInternal::providerReleaseQuiesced)
        {
            LOG_TRACE("sleeping for 500ms before reset()!");
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        if (deviceContext == State::Instance().currentFeature)
            State::Instance().currentFeature = nullptr;

        existingContext->second.feature.reset();
        if (!shutdown && !GenshinFsrInternal::providerReleaseQuiesced &&
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

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_EvaluateFeature(ID3D11DeviceContext* InDevCtx,
                                                               const NVSDK_NGX_Handle* InFeatureHandle,
                                                               NVSDK_NGX_Parameter* InParameters,
                                                               PFN_NVSDK_NGX_ProgressCallback InCallback)
{
    PersistentSrDx11::Entry poolEntry(PersistentSrDx11::Instance());
    if (!poolEntry) return NVSDK_NGX_Result_Fail;
    auto pooled = PersistentSrDx11::Instance().Lookup(InFeatureHandle);
    const bool pooledHandle = pooled.status != PersistentSrDx11::Status::Unknown;
    if (pooledHandle && (!InParameters || !PersistentSrDx11::PrepareEvaluate(pooled, InFeatureHandle, InDevCtx)))
        return NVSDK_NGX_Result_FAIL_FeatureNotFound;
    if (InFeatureHandle == nullptr)
    {
        PublishGenshinSrOutcome(InDevCtx,InParameters,InFeatureHandle,GenshinSrReceipt::Outcome::NoHandle);
        LOG_DEBUG("InFeatureHandle is null");
        return NVSDK_NGX_Result_FAIL_FeatureNotFound;
    }
    else
    {
        LOG_DEBUG("Handle: {0}", InFeatureHandle->Id);
    }

    if (InDevCtx == nullptr)
    {
        PublishGenshinSrOutcome(InDevCtx,InParameters,InFeatureHandle,GenshinSrReceipt::Outcome::InvalidContext);
        LOG_ERROR("InDevCtx is null!!!");
        return NVSDK_NGX_Result_Fail;
    }

    State& state = State::Instance();
    auto handleId = pooledHandle ? pooled.slot->backendId : InFeatureHandle->Id;
    if (!pooledHandle && PersistentSrDx11::Instance().OwnsId(handleId))
        return NVSDK_NGX_Result_FAIL_FeatureNotFound;
    if (handleId < DLSS_MOD_ID_OFFSET)
    {
        if (GenshinSrReceipt::active)
        {
            PublishGenshinSrOutcome(InDevCtx,InParameters,InFeatureHandle,GenshinSrReceipt::Outcome::UnsupportedFeature);
            return NVSDK_NGX_Result_Fail;
        }
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
        PublishGenshinSrOutcome(InDevCtx,InParameters,InFeatureHandle,GenshinSrReceipt::Outcome::SkipFirstFrames);
        evaluateObservation.Result(NVSDK_NGX_Result_Success);
        return NVSDK_NGX_Result_Success;
    }

    if (InCallback)
        LOG_INFO("callback exist");

    IFeature_Dx11* deviceContext = nullptr;
    auto activeContext = pooledHandle ? &pooled.slot->owner->context : &Dx11Contexts[handleId];

    if (!pooledHandle && state.changeBackend[handleId])
    {
        auto successfulPhase = FeatureProvider_Dx11::ChangeFeature(state.newBackend, D3D11Device, InDevCtx, handleId,
                                                                   InParameters, activeContext,
                                                                   !GenshinSrReceipt::active);

        evalCounter = 0;

        if (activeContext->changeBackendCounter != 0 || !successfulPhase)
        {
            // Only the internal Genshin receipt distinguishes a successful
            // incomplete phase from failure; public NGX return semantics stay unchanged.
            PublishGenshinSrOutcome(InDevCtx,InParameters,InFeatureHandle,successfulPhase
                ? GenshinSrReceipt::Outcome::BackendRebuild : GenshinSrReceipt::Outcome::EvaluateFailed);
            evaluateObservation.Result(NVSDK_NGX_Result_Success);
            return NVSDK_NGX_Result_Success;
        }
    }

    if (activeContext->feature == nullptr) // prevent source api name flicker when dlssg is active
    {
        state.setInputApiName = state.currentInputApiName;
    }
    else
    {
        deviceContext = activeContext->feature.get();
        state.currentFeature = deviceContext;
    }

    const auto targetApiName =
        !state.setInputApiName.has_value() ? ApiUpscalerInput::DLSS_DX11 : state.setInputApiName.value();

    if (state.currentInputApiName != targetApiName)
        state.currentInputApiName = targetApiName;

    state.setInputApiName.reset();

    if (deviceContext == nullptr)
    {
        PublishGenshinSrOutcome(InDevCtx,InParameters,InFeatureHandle,GenshinSrReceipt::Outcome::NoFeature);
        LOG_DEBUG("trying to use released handle, returning NVSDK_NGX_Result_Success");
        evaluateObservation.Result(NVSDK_NGX_Result_Success);
        return NVSDK_NGX_Result_Success;
    }

    const auto persistentAutoBefore = state.autoExposure;
    const auto persistentChangedBefore = state.changeBackend[handleId];
    const auto persistentBackendBefore = state.newBackend;
    const bool genshinBridge = GenshinSrReceipt::active && deviceContext->IsWithDx12();
    if (GenshinSrReceipt::active && (deviceContext->GetUpscalerType()==Upscaler::DLSSD ||
        (genshinBridge && !GenshinSrReceipt::active->BeginObservationTransport(InDevCtx,InParameters,
            Config::Instance()->AcquireGenshinControls().epoch))))
    {
        PublishGenshinSrOutcome(InDevCtx,InParameters,InFeatureHandle,GenshinSrReceipt::Outcome::UnsupportedFeature);
        evaluateObservation.Result(NVSDK_NGX_Result_Fail);
        return NVSDK_NGX_Result_Fail;
    }
    auto upscaleResult = pooledHandle ? PersistentSrDx11::Evaluate(pooled, InDevCtx, InParameters)
                                     : deviceContext->Evaluate(InDevCtx, InParameters);
    if (pooledHandle) PersistentSrDx11::ObserveEvaluate(*pooled.slot, persistentAutoBefore,
                                                       persistentChangedBefore, persistentBackendBefore);
    PublishGenshinSrOutcome(InDevCtx,InParameters,deviceContext->Handle(),
        upscaleResult ? GenshinSrReceipt::Outcome::ActualEvaluateSucceeded : GenshinSrReceipt::Outcome::EvaluateFailed,true);

    const auto upscaler = deviceContext->GetUpscalerType();
    // Persistent handles retain their separate admission/rebuild contract.
    const auto fallback = FeatureProvider_Dx11::FallbackUpscaler(upscaler,
        !pooledHandle && !GenshinSrReceipt::active);
    const bool bridgeFallback = fallback == Upscaler::FSR22_on12;

    // A failed ordinary bridge evaluation must not publish its cached guides as fresh.
    if (upscaleResult && GenshinSrReceipt::AllowsDownstreamGuides() &&
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
        state.newBackend = fallback;
        state.changeBackend[handleId] = true;
    }

    evaluateObservation.Result(NVSDK_NGX_Result_Success);
    return NVSDK_NGX_Result_Success;
}

#pragma endregion
