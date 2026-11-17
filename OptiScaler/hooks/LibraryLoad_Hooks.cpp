#include "pch.h"
#include <runtime/F6InputPolicy.h>
#include "LibraryLoad_Hooks.h"

#include <Config.h>
#include <DllNames.h>

#include <proxies/Ntdll_Proxy.h>
#include <proxies/Kernel32_Proxy.h>
#include <proxies/Dxgi_Proxy.h>
#include <proxies/D3D12_Proxy.h>

#include <proxies/XeSS_Proxy.h>
#include <proxies/XeFG_Proxy.h>
#include <proxies/XeLL_Proxy.h>
#include <proxies/NVNGX_Proxy.h>
#include <proxies/FfxApi_Proxy.h>

#include <inputs/FSR2_Dx12.h>
#include <inputs/FSR3_Dx12.h>
#include <inputs/FfxApiExe_Dx12.h>

#include <spoofing/Dxgi_Spoofing.h>

#include <hooks/Dxgi_Hooks.h>
#include <hooks/D3D11_Hooks.h>
#include <hooks/D3D12_Hooks.h>
#include <hooks/Vulkan_Hooks.h>
#include <hooks/Gdi32_Hooks.h>
#include <hooks/Streamline_Hooks.h>
#include <hooks/ngx/NativeDx12Hooks.h>

#include <fsr4/FSR4ModelSelection.h>
#include <fsr4/FSR4Upgrade.h>
#include <misc/IdentifyGpu.h>
#include <low_latency/input/input_uell.h>
#include <runtime/ModuleLifetime.h>
#include <runtime/LibraryObservation.h>

// #define LOG_LIB_OPERATIONS

namespace {
std::wstring LibraryFilename(std::wstring_view path) {
    const auto separator = path.find_last_of(L"\\/");
    std::wstring name(path.substr(separator == std::wstring_view::npos ? 0 : separator+1));
    to_lower_in_place(name);
    return name;
}
bool Named(const std::wstring& name, const std::vector<std::wstring>& names) {
    return std::ranges::find(names, name) != names.end();
}
std::wstring ModulePath(HMODULE module) {
    std::wstring path(32768, L'\0');
    const auto length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return {};
    path.resize(length); return path;
}
LibraryLoading::Observations observations;
// One process-lifetime timer, initialized outside loader callouts. Pending
// failures are retried without another unrelated library load or a busy loop.
PTP_TIMER observationRetryTimer = nullptr;
std::atomic<bool> observationWorker {false};
void RunObservationWorker();
const auto& ModuleFamilies() {
    static const std::vector<std::wstring>* const families[]{
        &nvngxNamesW, &nvapiNamesW, &dx11NamesW, &dx12NamesW, &dx12agilityNamesW, &dxgiNamesW, &vkNamesW,
        &slInterposerNamesW, &slDlssNamesW, &slDlssgNamesW, &slReflexNamesW, &slPclNamesW, &slCommonNamesW,
        &fsr2NamesW, &fsr2BENamesW, &fsr3NamesW, &fsr3BENamesW, &ffxDx12NamesW, &ffxDx12UpscalerNamesW,
        &ffxDx12FGNamesW, &ffxDx12DenoiserNamesW, &ffxDx12RadianceNamesW, &ffxVkNamesW, &xessNamesW, &xessDx11NamesW, &amdxc64NamesW, &uellNamesW
    };
    return families;
}
enum class OtaPlugin { None, Dlss, Dlssg, Reflex, Pcl, Common };
OtaPlugin OtaRole(std::wstring_view path) {
    if (!path.ends_with(L".dll") || !path.contains(L"\\versions\\")) return OtaPlugin::None;
    if (path.contains(L"\\sl_dlss_0")) return OtaPlugin::Dlss;
    if (path.contains(L"\\sl_dlss_g_")) return OtaPlugin::Dlssg;
    if (path.contains(L"\\sl_reflex_")) return OtaPlugin::Reflex;
    if (path.contains(L"\\sl_pcl_")) return OtaPlugin::Pcl;
    if (path.contains(L"\\sl_common_")) return OtaPlugin::Common;
    return OtaPlugin::None;
}
bool InterestingModule(std::wstring path) {
    to_lower_in_place(path);
    const auto name=LibraryFilename(path);
    for (const auto* family : ModuleFamilies()) if (Named(name,*family)) return true;
    return name.ends_with(L".bin") || OtaRole(path)!=OtaPlugin::None;
}
}

LibraryLoadHooks::LoadPlan LibraryLoadHooks::Decide(std::wstring_view requested)
{
    LoadPlan plan;
    if (State::Instance().isShuttingDown || IsApiSetName(requested)) return plan;
    if (State::SkipDllChecks()) {
        const auto skip = string_to_wstring(State::SkipDllName());
        if (skip.empty() || EndsWithInsensitive(requested, skip) || EndsWithInsensitive(requested, skip+L".dll")) return plan;
    }
    const std::wstring fullPath(requested);
    auto normalized = std::filesystem::path(fullPath).lexically_normal().wstring();
    to_lower_in_place(normalized);
    const auto name = LibraryFilename(normalized);
    const auto& state = State::Instance();
    const bool externalFg = state.externalFrameGeneration &&
        ((name.starts_with(L"sl.") && name.ends_with(L".dll")) || name == L"nvngx_dlssg.dll" ||
         (normalized.contains(L"\\versions\\") && (normalized.contains(L"\\sl_") || normalized.contains(L"\\dlssg\\"))));
    if (externalFg) return plan;
    if (auto isolated = StreamlineHooks::IsolatedGamePluginPath(fullPath.c_str()); !isolated.empty()) {
        plan.action = LoadAction::RedirectPath; plan.replacement = std::move(isolated); return plan;
    }
    auto exePath = Util::ExePath().parent_path().wstring(); to_lower_in_place(exePath);
    if (Config::Instance()->EnableDlssInputs.value_or_default() && Named(name, nvngxNamesW) &&
        (!Config::Instance()->HookOriginalNvngxOnly.value_or_default() || normalized.find(exePath) == std::wstring::npos)) {
        if (!NgxInput::Enabled()) { plan.action=LoadAction::AliasModule; plan.alias=dllModule; }
        return plan;
    }
    if ((state.workingMode != WorkingMode::Dxgi || !state.skipDxgiLoadChecks) && Named(name,dllNamesW)) {
        plan.action=LoadAction::AliasModule; plan.alias=state.ServeOriginal() ? originalModule : dllModule; return plan;
    }
    if (Config::Instance()->DLSSEnabled.value_or_default() && Config::Instance()->NVNGX_DLSS_Library.has_value() &&
        Named(name,nvngxDlssNamesW)) {
        plan.action=LoadAction::RedirectPath; plan.replacement=Config::Instance()->NVNGX_DLSS_Library.value();
        plan.fallbackToOriginal=true; return plan;
    }
    if (Named(name,nvapiNamesW) && Config::Instance()->UseFakenvapi.value_or_default()) {
        const auto* gpus=IdentifyGpu::getLoaderRoutingSnapshot();
        const auto vendor=gpus && !gpus->empty() ? gpus->front().vendorId : VendorId::Invalid;
        if (vendor != VendorId::Invalid && vendor != VendorId::Nvidia) {
            plan.action=LoadAction::AliasModule; plan.alias=dllModule;
        } else plan.fallbackAlias=dllModule;
        return plan;
    }
    if ((!Config::Instance()->DisableOverlays.has_value() && state.activeFgOutput != FGOutput::NoFG && Named(name,eosOverlayNamesW)) ||
        Named(name,blockedDllNamesW) || (Config::Instance()->DisableOverlays.value_or_default() && Named(name,blockOverlayNamesW))) {
        plan.action=LoadAction::Reject; return plan;
    }
    const auto redirect = [&](const std::filesystem::path& replacement) {
        if (!replacement.empty()) { plan.action=LoadAction::RedirectPath; plan.replacement=replacement.wstring(); }
    };
    if (Named(name,xessNamesW) && XeSSProxy::Module()) redirect(XeSSProxy::Module_Path());
    else if (Named(name,xessDx11NamesW) && XeSSProxy::ModuleDx11()) redirect(XeSSProxy::ModuleDx11_Path());
    else if (F6InputPolicy::PublicFsrInputs && Named(name,ffxDx12NamesW) && FfxApiProxy::Dx12Module()) redirect(FfxApiProxy::Dx12Module_Path());
    else if (F6InputPolicy::PublicFsrInputs && Named(name,ffxDx12UpscalerNamesW) && FfxApiProxy::Dx12Module_SR()) redirect(FfxApiProxy::Dx12Module_SR_Path());
    else if (F6InputPolicy::PublicFsrInputs && Named(name,ffxDx12FGNamesW) && FfxApiProxy::Dx12Module_FG()) redirect(FfxApiProxy::Dx12Module_FG_Path());
    else if (F6InputPolicy::PublicFsrInputs && Named(name,ffxVkNamesW) && FfxApiProxy::VkModule()) redirect(FfxApiProxy::VkModule_Path());
    else if (Named(name,amdxc64NamesW)) {
        if (Config::Instance()->LoadCustomAmdxc64OnRdna2.value_or_default()) {
            const auto* gpus=IdentifyGpu::getLoaderRoutingSnapshot();
            if (gpus) for (const auto& gpu : *gpus) if (Util::IsSubpath(std::filesystem::path(fullPath),gpu.driverStore)) {
                if (gpu.amdHwGeneration == device_info::HwGeneration::kGfx10_3) {
                    auto replacement=std::filesystem::path(Config::Instance()->MainDllPath.value());
                    if (std::filesystem::is_directory(replacement)) replacement /= L"amdxc64.dll";
                    if (std::filesystem::exists(replacement)) {
                        redirect(replacement);
                        plan.fallbackToOriginal=!Config::Instance()->Fsr4DoNotLoadAmdxc64.value_or_default();
                    }
                }
                break;
            }
        }
        if (plan.action == LoadAction::Forward && Config::Instance()->Fsr4DoNotLoadAmdxc64.value_or_default())
            plan.action=LoadAction::Reject;
    }
    return plan;
}

HMODULE LibraryLoadHooks::AcquireAlias(std::wstring_view requested, HMODULE module)
{
    HMODULE result=nullptr;
    if (!module || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(module), &result)) return nullptr;
    if (module==dllModule && Named(LibraryFilename(requested),nvapiNamesW)) fakenvapi::setUsingAsMainNvapi(true);
    return result;
}

void LibraryLoadHooks::ObserveExecutable(HMODULE module, std::wstring_view requested)
{
    if (!module || (reinterpret_cast<uintptr_t>(module)&3) || State::Instance().isShuttingDown) return;
    const auto savedError=GetLastError();
    struct RestoreError { DWORD value; ~RestoreError() { SetLastError(value); } } restore{savedError};
    LibraryLoading::InternalLoad::notify.store(SchedulePending,std::memory_order_release);
    const bool nvapiRole=Named(LibraryFilename(requested),nvapiNamesW);
    observations.RequestScan();
    if (!nvapiRole && !InterestingModule(ModulePath(module))) { ProcessPending(); SchedulePending(); return; }
    observations.Enqueue(module,nvapiRole);
    ProcessPending();
    SchedulePending();
}

void LibraryLoadHooks::ProcessPending()
{
    const auto savedError=GetLastError();
    struct RestoreError { DWORD value; ~RestoreError() { SetLastError(value); } } restore{savedError};
    observations.Process(!LibraryLoading::InternalLoad::current && !State::Instance().isShuttingDown &&
        ModuleLifetime::CurrentContext()==LoaderStartupContext::Status::Ready,
        [] { CheckModulesInMemory(); }, [](HMODULE module, bool role) { return ObserveNow(module,role); },
        [](HMODULE module, bool nvapi) {
            using Ownership=LibraryLoading::ObservationOwnership;
            if (nvapi) return Ownership::Hooks;
            auto path=ModulePath(module); to_lower_in_place(path);
            const auto name=LibraryFilename(path);
            if (State::Instance().externalFrameGeneration &&
                ((name.starts_with(L"sl.") && name.ends_with(L".dll")) || name==L"nvngx_dlssg.dll" ||
                 (path.contains(L"\\versions\\") && (path.contains(L"\\sl_") || path.contains(L"\\dlssg\\"))))) return Ownership::Ignore;
            if (name.ends_with(L".bin")) return Ownership::Metadata;
            return Ownership::Hooks;
        });
}

namespace {
void RunObservationWorker() {
    for (;;) {
        LibraryLoadHooks::ProcessPending();
        if (!State::Instance().isShuttingDown && observations.HasWork()) {
            if (!observationRetryTimer) observationRetryTimer=CreateThreadpoolTimer(
                [](PTP_CALLBACK_INSTANCE,void*,PTP_TIMER) { RunObservationWorker(); },nullptr,nullptr);
            if (observationRetryTimer) {
                LARGE_INTEGER delay{}; delay.QuadPart=-1'000'000; // 100 ms, no loader-lock wait
                FILETIME due{delay.LowPart,static_cast<DWORD>(delay.HighPart)};
                SetThreadpoolTimer(observationRetryTimer,&due,0,0);
                return; // the scheduled flag remains owned by this timer
            }
            Sleep(100); // allocation failure: retain the single safe-context worker
            continue;
        }
        observationWorker.store(false,std::memory_order_release);
        // A producer may have enqueued while the scheduled flag was still set.
        if (!State::Instance().isShuttingDown && observations.HasWork()) LibraryLoadHooks::SchedulePending();
        return;
    }
}
}
void LibraryLoadHooks::SchedulePending() noexcept
{
    const auto savedError=GetLastError();
    if (observations.HasWork() && !State::Instance().isShuttingDown &&
        !observationWorker.exchange(true,std::memory_order_acq_rel)) {
        if (!QueueUserWorkItem([](void*) -> DWORD { RunObservationWorker(); return 0; }, nullptr, WT_EXECUTEDEFAULT))
            observationWorker.store(false,std::memory_order_release);
    }
    SetLastError(savedError);
}

bool LibraryLoadHooks::ObserveNow(HMODULE module, bool nvapiRole)
{
    const auto fullPath=ModulePath(module);
    if (fullPath.empty()) return false;
    bool completed = true;
    const auto name=LibraryFilename(fullPath);
    auto normalized=std::filesystem::path(fullPath).lexically_normal().wstring(); to_lower_in_place(normalized);
    const auto ota=OtaRole(normalized);
    auto& state=State::Instance();
    const bool externalFg=state.externalFrameGeneration &&
        ((name.starts_with(L"sl.")&&name.ends_with(L".dll")) || name==L"nvngx_dlssg.dll" ||
        (normalized.contains(L"\\versions\\")&&(normalized.contains(L"\\sl_")||normalized.contains(L"\\dlssg\\"))));
    if (!externalFg) {
        const auto local=std::filesystem::path(Config::Instance()->MainDllPath.value())/L"streamline";
        const bool inside=Util::IsSubpath(std::filesystem::path(fullPath),local.lexically_normal());
        const bool hookSl=!inside || state.activeFgInput==FGInput::NvngxFG;
        if (Named(name,nvngxNamesW)&&NgxInput::Enabled()&&module!=dllModule) NVNGXProxy::InitNVNGX(module);
        if (name.ends_with(L".bin")&&normalized.contains(L"\\versions\\")) {
            if (normalized.contains(L"\\dlss\\")) state.NGX_OTA_Dlss=wstring_to_string(fullPath);
            if (normalized.contains(L"\\dlssd\\")) state.NGX_OTA_Dlssd=wstring_to_string(fullPath);
        }
        if (hookSl&&Named(name,slInterposerNamesW)&&module!=state.optiSlInterposer) {
            completed = StreamlineHooks::hookInterposer(module) && completed; slInterposerModule=module;
        }
        if (hookSl&&(Named(name,slDlssNamesW)||ota==OtaPlugin::Dlss)) completed = StreamlineHooks::hookDlss(module) && completed;
        if (Named(name,slDlssgNamesW)||ota==OtaPlugin::Dlssg) {
            if (inside&&state.activeFgOutput==FGOutput::DLSSG) completed = StreamlineHooks::hookLocalDlssg(module) && completed;
            else if (module!=state.optiSlDLSSG) completed = StreamlineHooks::hookDlssg(module) && completed;
        }
        if (hookSl&&(Named(name,slReflexNamesW)||ota==OtaPlugin::Reflex)&&module!=state.optiSlReflex) completed = StreamlineHooks::hookReflex(module) && completed;
        if (hookSl&&(Named(name,slPclNamesW)||ota==OtaPlugin::Pcl)&&module!=state.optiSlPCL) completed = StreamlineHooks::hookPcl(module) && completed;
        if (hookSl&&(Named(name,slCommonNamesW)||ota==OtaPlugin::Common)&&module!=state.optiSlCommon) completed = StreamlineHooks::hookCommon(module) && completed;
    }
    if (nvapiRole || Named(name,nvapiNamesW)) NvApiHooks::Hook(module);
    if (Named(name,dx11NamesW)) completed = D3D11Hooks::Hook(module) && completed;
    if (Named(name,dx12NamesW)) { D3d12Proxy::Init(module); completed = D3D12Hooks::Hook() && completed; }
    if (Named(name,dx12agilityNamesW)) completed = D3D12Hooks::HookAgility(module) && completed;
    if (Named(name,vkNamesW)) {
        completed = VulkanHooks::Hook(module) && completed;
    }
    if (!state.skipDxgiLoadChecks&&Named(name,dxgiNamesW)) { DxgiProxy::Init(module); DxgiHooks::Hook(); }
    if (F6InputPolicy::PublicFsrInputs) {
        if (Named(name,fsr2NamesW)) HookFSR2Inputs(module);
        if (Named(name,fsr2BENamesW)) HookFSR2Dx12Inputs(module);
        if (Named(name,fsr3NamesW)) HookFSR3Inputs(module);
        if (Named(name,fsr3BENamesW)) HookFSR3Dx12Inputs(module);
        if (Named(name,ffxDx12NamesW)) FfxApiProxy::InitFfxDx12(module);
        if (Named(name,ffxDx12UpscalerNamesW)) FfxApiProxy::InitFfxDx12_SR(module);
        if (Named(name,ffxDx12FGNamesW)) FfxApiProxy::InitFfxDx12_FG(module);
        if (Named(name,ffxDx12DenoiserNamesW)) FfxApiProxy::InitFfxDx12_Denoiser(module);
        if (Named(name,ffxDx12RadianceNamesW)) FfxApiProxy::InitFfxDx12_Radiance(module);
        if (Named(name,ffxVkNamesW)) FfxApiProxy::InitFfxVk(module);
    }
    if (Named(name,xessNamesW)) XeSSProxy::InitXeSS(module);
    if (Named(name,xessDx11NamesW)) XeSSProxy::InitXeSSDx11(module);
    if (Named(name,amdxc64NamesW)) Amdxc64Hooks::Init(module);
    if (Named(name,uellNamesW)) {
        auto start=reinterpret_cast<PFN_setTickCallback>(KernelBaseProxy::GetProcAddress_()(module,"setTickStartCallback"));
        auto end=reinterpret_cast<PFN_setTickCallback>(KernelBaseProxy::GetProcAddress_()(module,"setTickEndCallback"));
        if(start)start(InputUeLowLatency::tickStart); if(end)end(InputUeLowLatency::tickEnd);
    }
    return completed;
}

std::optional<NTSTATUS> LibraryLoadHooks::FreeLibrary(PVOID lpLibrary)
{
    std::optional<NTSTATUS> result;

    if (lpLibrary == dllModule)
    {
#ifdef LOG_LIB_OPERATIONS
        LOG_WARN("Call for OptiScaler, caller: {}", Util::WhoIsTheCaller(_ReturnAddress()));
#endif
        result = TRUE;
    }
    else if (lpLibrary == FfxApiProxy::Dx12Module())
    {
#ifdef LOG_LIB_OPERATIONS
        LOG_WARN("Call for FFX Dx12, caller: {}", Util::WhoIsTheCaller(_ReturnAddress()));
#endif
        result = TRUE;
    }
    else if (lpLibrary == FfxApiProxy::VkModule())
    {
#ifdef LOG_LIB_OPERATIONS
        LOG_WARN("Call for FFX Vulkan, caller: {}", Util::WhoIsTheCaller(_ReturnAddress()));
#endif
        result = TRUE;
    }
    else if (lpLibrary == XeSSProxy::Module())
    {
#ifdef LOG_LIB_OPERATIONS
        LOG_WARN("Call for XeSS, caller: {}", Util::WhoIsTheCaller(_ReturnAddress()));
#endif
        result = TRUE;
    }
    else if (lpLibrary == DxgiProxy::Module())
    {
#ifdef LOG_LIB_OPERATIONS
        LOG_WARN("Call for DXGI, caller: {}", Util::WhoIsTheCaller(_ReturnAddress()));
#endif
        result = TRUE;
    }
    else if (lpLibrary == D3d12Proxy::Module())
    {
#ifdef LOG_LIB_OPERATIONS
        LOG_WARN("Call for D3D12, caller: {}", Util::WhoIsTheCaller(_ReturnAddress()));
#endif
        result = TRUE;
    }
    else if (lpLibrary == Kernel32Proxy::Module())
    {
#ifdef LOG_LIB_OPERATIONS
        LOG_WARN("Call for Kernel32, caller: {}", Util::WhoIsTheCaller(_ReturnAddress()));
#endif
        result = TRUE;
    }
    else if (lpLibrary == KernelBaseProxy::Module())
    {
#ifdef LOG_LIB_OPERATIONS
        LOG_WARN("Call for KernelBase, caller: {}", Util::WhoIsTheCaller(_ReturnAddress()));
#endif
        result = TRUE;
    }
    else if (lpLibrary == NtdllProxy::Module())
    {
#ifdef LOG_LIB_OPERATIONS
        LOG_WARN("Call for ntdll, caller: {}", Util::WhoIsTheCaller(_ReturnAddress()));
#endif
        result = TRUE;
    }
    else if (lpLibrary == vulkanModule)
    {
#ifdef LOG_LIB_OPERATIONS
        LOG_WARN("Call for Vulkan, caller: {}", Util::WhoIsTheCaller(_ReturnAddress()));
#endif
        result = TRUE;
    }
    else if (lpLibrary == d3d11Module)
    {
#ifdef LOG_LIB_OPERATIONS
        LOG_WARN("Call for D3D11, caller: {}", Util::WhoIsTheCaller(_ReturnAddress()));
#endif
        result = TRUE;
    }
    return result;
}

HMODULE LibraryLoadHooks::LoadNvApi()
{
    LOG_FUNC();

    HMODULE nvapi = nullptr;

    // Opti exports a query function that nvapi would export. Early DXGI
    // discovery can temporarily return VendorId::Invalid during DLL attach;
    // unknown is not proof of a non-NVIDIA GPU. Selecting fakenvapi here would
    // permanently hide the real architecture query from Streamline/DLSS even
    // after the actual NVIDIA adapter is discovered.
    const auto* gpus = IdentifyGpu::getLoaderRoutingSnapshot();
    const auto vendor = gpus && !gpus->empty() ? gpus->front().vendorId : VendorId::Invalid;
    const bool knownNonNvidia = vendor != VendorId::Invalid && vendor != VendorId::Nvidia;
    if (Config::Instance()->UseFakenvapi.value_or_default() && knownNonNvidia)
    {
        nvapi = dllModule;
        fakenvapi::setUsingAsMainNvapi(true);

        if (GetModuleHandleW(L"nvapi64.dll"))
            LOG_WARN("nvapi64.dll is loaded when Nvidia is not the primary GPU");
    }
    else if (Config::Instance()->UseFakenvapi.value_or_default() && vendor == VendorId::Invalid)
    {
        LOG_INFO("GPU identity is not ready; trying the real system NVAPI before considering fakenvapi");
    }

    // Try to load nvapi only from system32, like the original call would
    if (nvapi == nullptr)
        nvapi = NtdllProxy::LoadLibraryExW_Ldr(L"nvapi64.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);

    // Fallback for Nvidia, doubt it's helpful
    if (Config::Instance()->UseFakenvapi.value_or_default() && nvapi == nullptr)
    {
        LOG_WARN("Using fakenvapi on Nvidia as the main nvapi");
        nvapi = dllModule;
        fakenvapi::setUsingAsMainNvapi(true);
    }

    if (nvapi != nullptr)
        ObserveExecutable(nvapi, L"nvapi64.dll");

    return nvapi;
}

HMODULE LibraryLoadHooks::LoadNvngxDlss(std::wstring originalPath)
{
    HMODULE nvngxDlss = nullptr;

    if (Config::Instance()->NVNGX_DLSS_Library.has_value())
    {
        nvngxDlss = NtdllProxy::LoadLibraryExW_Ldr(Config::Instance()->NVNGX_DLSS_Library.value().c_str(), NULL, 0);

        if (nvngxDlss != nullptr)
        {
            LOG_INFO("nvngx_dlss.dll loaded from {0}",
                     wstring_to_string(Config::Instance()->NVNGX_DLSS_Library.value()));
            return nvngxDlss;
        }
        else
        {
            LOG_WARN("nvngx_dlss.dll can't found at {0}",
                     wstring_to_string(Config::Instance()->NVNGX_DLSS_Library.value()));
        }
    }

    if (nvngxDlss == nullptr)
    {
        nvngxDlss = NtdllProxy::LoadLibraryExW_Ldr(originalPath.c_str(), NULL, 0);

        if (nvngxDlss != nullptr)
        {
            LOG_INFO("nvngx_dlss.dll loaded from {0}", wstring_to_string(originalPath));
            return nvngxDlss;
        }
    }

    return nullptr;
}

// HMODULE LibraryLoadHooks::LoadLibxess(std::wstring originalPath)
//{
//     if (XeSSProxy::Module() != nullptr)
//         return XeSSProxy::Module();
//
//     HMODULE libxess = nullptr;
//
//     if (Config::Instance()->XeSSLibrary.has_value())
//     {
//         std::filesystem::path libPath(Config::Instance()->XeSSLibrary.value().c_str());
//
//         if (libPath.has_filename())
//             libxess = NtdllProxy::LoadLibraryExW_Ldr(libPath.c_str(), NULL, 0);
//         else
//             libxess = NtdllProxy::LoadLibraryExW_Ldr((libPath / L"libxess.dll").c_str(), NULL, 0);
//
//         if (libxess != nullptr)
//         {
//             LOG_INFO("libxess.dll loaded from {0}", wstring_to_string(Config::Instance()->XeSSLibrary.value()));
//             return libxess;
//         }
//         else
//         {
//             LOG_WARN("libxess.dll can't found at {0}", wstring_to_string(Config::Instance()->XeSSLibrary.value()));
//         }
//     }
//
//     if (libxess == nullptr)
//     {
//         libxess = NtdllProxy::LoadLibraryExW_Ldr(originalPath.c_str(), NULL, 0);
//
//         if (libxess != nullptr)
//         {
//             LOG_INFO("libxess.dll loaded from {0}", wstring_to_string(originalPath));
//             return libxess;
//         }
//     }
//
//     return nullptr;
// }
//
// HMODULE LibraryLoadHooks::LoadLibxessDx11(std::wstring originalPath)
//{
//     if (XeSSProxy::ModuleDx11() != nullptr)
//         return XeSSProxy::ModuleDx11();
//
//     HMODULE libxess = nullptr;
//
//     if (Config::Instance()->XeSSDx11Library.has_value())
//     {
//         std::filesystem::path libPath(Config::Instance()->XeSSDx11Library.value().c_str());
//
//         if (libPath.has_filename())
//             libxess = NtdllProxy::LoadLibraryExW_Ldr(libPath.c_str(), NULL, 0);
//         else
//             libxess = NtdllProxy::LoadLibraryExW_Ldr((libPath / L"libxess_dx11.dll").c_str(), NULL, 0);
//
//         if (libxess != nullptr)
//         {
//             LOG_INFO("libxess_dx11.dll loaded from {0}",
//                      wstring_to_string(Config::Instance()->XeSSDx11Library.value()));
//             return libxess;
//         }
//         else
//         {
//             LOG_WARN("libxess_dx11.dll can't found at {0}",
//                      wstring_to_string(Config::Instance()->XeSSDx11Library.value()));
//         }
//     }
//
//     if (libxess == nullptr)
//     {
//         libxess = NtdllProxy::LoadLibraryExW_Ldr(originalPath.c_str(), NULL, 0);
//
//         if (libxess != nullptr)
//         {
//             LOG_INFO("libxess_dx11.dll loaded from {0}", wstring_to_string(originalPath));
//             return libxess;
//         }
//     }
//
//     return nullptr;
// }
//
// HMODULE LibraryLoadHooks::LoadFfxapiDx12(std::wstring originalPath)
//{
//     if (FfxApiProxy::Dx12Module() != nullptr)
//         return FfxApiProxy::Dx12Module();
//
//     HMODULE ffxDx12 = nullptr;
//
//     std::vector<std::wstring> dllNames = { L"amd_fidelityfx_loader_dx12.dll", L"amd_fidelityfx_dx12.dll" };
//
//     for (size_t i = 0; i < dllNames.size(); i++)
//     {
//         if (Config::Instance()->FfxDx12Path.has_value())
//         {
//             std::filesystem::path libPath(Config::Instance()->FfxDx12Path.value().c_str());
//
//             if (libPath.has_filename())
//                 ffxDx12 = NtdllProxy::LoadLibraryExW_Ldr(libPath.c_str(), NULL, 0);
//             else
//                 ffxDx12 = NtdllProxy::LoadLibraryExW_Ldr((libPath / dllNames[i]).c_str(), NULL, 0);
//
//             if (ffxDx12 != nullptr)
//             {
//                 LOG_INFO("{0} loaded from {1}", wstring_to_string(dllNames[i]),
//                          wstring_to_string(Config::Instance()->FfxDx12Path.value()));
//                 return ffxDx12;
//             }
//             else
//             {
//                 LOG_WARN("{0} can't found at {1}", wstring_to_string(dllNames[i]),
//                          wstring_to_string(Config::Instance()->FfxDx12Path.value()));
//             }
//         }
//
//         if (ffxDx12 == nullptr)
//         {
//             ffxDx12 = NtdllProxy::LoadLibraryExW_Ldr(originalPath.c_str(), NULL, 0);
//
//             if (ffxDx12 != nullptr)
//             {
//                 LOG_INFO("{0} loaded from {1}", wstring_to_string(dllNames[i]), wstring_to_string(originalPath));
//                 return ffxDx12;
//             }
//         }
//     }
//
//     return nullptr;
// }
//
// HMODULE LibraryLoadHooks::LoadFfxapiVk(std::wstring originalPath)
//{
//     if (FfxApiProxy::VkModule() != nullptr)
//         return FfxApiProxy::VkModule();
//
//     HMODULE ffxVk = nullptr;
//
//     if (Config::Instance()->FfxVkPath.has_value())
//     {
//         std::filesystem::path libPath(Config::Instance()->FfxVkPath.value().c_str());
//
//         if (libPath.has_filename())
//             ffxVk = NtdllProxy::LoadLibraryExW_Ldr(libPath.c_str(), NULL, 0);
//         else
//             ffxVk = NtdllProxy::LoadLibraryExW_Ldr((libPath / L"amd_fidelityfx_vk.dll").c_str(), NULL, 0);
//
//         if (ffxVk != nullptr)
//         {
//             LOG_INFO("amd_fidelityfx_vk.dll loaded from {0}",
//             wstring_to_string(Config::Instance()->FfxVkPath.value())); return ffxVk;
//         }
//         else
//         {
//             LOG_WARN("amd_fidelityfx_vk.dll can't found at {0}",
//                      wstring_to_string(Config::Instance()->FfxVkPath.value()));
//         }
//     }
//
//     if (ffxVk == nullptr)
//     {
//         ffxVk = NtdllProxy::LoadLibraryExW_Ldr(originalPath.c_str(), NULL, 0);
//
//         if (ffxVk != nullptr)
//         {
//             LOG_INFO("amd_fidelityfx_vk.dll loaded from {0}", wstring_to_string(originalPath));
//             return ffxVk;
//         }
//     }
//
//     return nullptr;
// }

void LibraryLoadHooks::CheckModulesInMemory()
{
    // Visit completed loader entries, not arbitrary images with a DLL name.
    // Static imports are reconciled even when no direct backend load was seen.
    LibraryLoading::VisitInitialized([](HMODULE module, std::wstring_view name, void*) {
        if (InterestingModule(ModulePath(module))) observations.Discover(module);
    }, nullptr);
}

bool LibraryLoadHooks::EndsWithInsensitive(std::wstring_view text, std::wstring_view suffix)
{
    if (suffix.size() > text.size())
        return false;

    if (suffix.empty())
        return true;

    const wchar_t* tail = text.data() + (text.size() - suffix.size());
    const int res =
        CompareStringOrdinal(tail, static_cast<int>(suffix.size()), suffix.data(), static_cast<int>(suffix.size()),
                             TRUE); // case-insensitive
    return res == CSTR_EQUAL;
}

bool LibraryLoadHooks::EndsWithInsensitive(const UNICODE_STRING& text, std::wstring_view suffix)
{
    return EndsWithInsensitive(std::wstring_view { text.Buffer, static_cast<size_t>(text.Length) / sizeof(wchar_t) },
                               suffix);
}

bool LibraryLoadHooks::StartsWithInsensitive(std::wstring_view str, std::wstring_view prefix)
{
    if (str.size() < prefix.size())
        return false;

    for (size_t i = 0; i < prefix.size(); ++i)
    {
        if (std::towlower(str[i]) != std::towlower(prefix[i]))
            return false;
    }

    return true;
}

bool LibraryLoadHooks::IsApiSetName(const std::wstring_view& n)
{
    return StartsWithInsensitive(n, L"api-ms-win-") || StartsWithInsensitive(n, L"ext-ms-") ||
           StartsWithInsensitive(n, L"api-ms-onecore-");
}
