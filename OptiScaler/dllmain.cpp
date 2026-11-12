#include "pch.h"
#include <runtime/F6InputPolicy.h>
#include <runtime/ModuleLifetime.h>
#include "integration/HoYoShade.h"
#include <genshin/GenshinNativeQueryProvenance.h>
#include "dllmain.h"

#include "Util.h"
#include "Config.h"
#include "Logger.h"
#include "StartupExceptionEvidence.h"
#include "resource.h"
#include "DllNames.h"

#include "proxies/Dxgi_Proxy.h"
#include "proxies/Kernel32_Proxy.h"
#include "proxies/KernelBase_Proxy.h"
#include "proxies/Ntdll_Proxy.h"
#include <proxies/IGDExt_Proxy.h>

#include <proxies/XeSS_Proxy.h>
#include <proxies/XeFG_Proxy.h>
#include <proxies/XeLL_Proxy.h>
#include <proxies/NVNGX_Proxy.h>
#include <proxies/FfxApi_Proxy.h>

#include "inputs/FSR2_Dx11.h"
#include "inputs/GenshinFsrContract.h"
#include "inputs/FSR2_Dx12.h"
#include "inputs/FSR2_Vk.h"
#include "inputs/FSR3_Dx12.h"
#include "inputs/FG/FSR3_Dx12_FG.h"

#include <fsr4/FSR4ModelSelection.h>

#include <hooks/Dxgi_Hooks.h>
#include <hooks/D3D11_Hooks.h>
#include <hooks/D3D12_Hooks.h>
#include <hooks/Vulkan_Hooks.h>
#include <hooks/Ntdll_Hooks.h>
#include <hooks/Kernel_Hooks.h>
#include <hooks/Gdi32_Hooks.h>
#include <hooks/Wintrust_Hooks.h>
#include <hooks/Crypt32_Hooks.h>
#include <hooks/Advapi32_Hooks.h>
#include <hooks/Streamline_Hooks.h>

#include <nvapi/NvApiHooks.h>

#include "spoofing/User32_Spoofing.h"
#include <misc/REFrameworkCompat.h>

#include <cwctype>
#include <magic_enum.hpp>
#include <version_check.h>
#include <misc/IdentifyGpu.h>
#include <sha1/sha1.hpp>

// R040 private startup diagnostic, implemented beside the configuration parser.
void LogHoYoShadeStartupConfig(bool enabled, unsigned stage) noexcept;

static std::vector<HMODULE> _asiHandles;
static std::vector<std::filesystem::directory_entry> _lateLoadingEntries;
static bool _passThruMode = false;

typedef const char*(CDECL* PFN_wine_get_version)(void);
typedef void (*PFN_InitializeASI)(void);
typedef bool (*PFN_PatchResult)(void);

static inline void* ManualGetProcAddress(HMODULE hModule, const char* functionName)
{
    if (!hModule)
        return nullptr;

    // Verify the alignment
    auto dosHeader = (IMAGE_DOS_HEADER*) hModule;
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE)
        return nullptr;

    auto ntHeaders = (IMAGE_NT_HEADERS*) ((BYTE*) hModule + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE)
        return nullptr;

    // Look at the export directory
    IMAGE_DATA_DIRECTORY exportData = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!exportData.VirtualAddress)
        return nullptr;

    auto exportDir = (IMAGE_EXPORT_DIRECTORY*) ((BYTE*) hModule + exportData.VirtualAddress);

    DWORD* nameRvas = (DWORD*) ((BYTE*) hModule + exportDir->AddressOfNames);
    WORD* ordinalTable = (WORD*) ((BYTE*) hModule + exportDir->AddressOfNameOrdinals);
    DWORD* functionTable = (DWORD*) ((BYTE*) hModule + exportDir->AddressOfFunctions);

    // Iterate over exported names
    for (DWORD i = 0; i < exportDir->NumberOfNames; ++i)
    {
        const char* name = (const char*) hModule + nameRvas[i];
        if (_stricmp(name, functionName) == 0)
        {
            WORD ordinal = ordinalTable[i];
            DWORD funcRva = functionTable[ordinal];
            return (BYTE*) hModule + funcRva;
        }
    }

    return nullptr; // Not found
}

static bool IsRunningOnWine()
{
    LOG_FUNC();

    HMODULE ntdll = GetModuleHandle(L"ntdll.dll");

    if (!ntdll)
    {
        LOG_WARN("Not running on NT!?!");
        return true;
    }

    auto pWineGetVersion = (PFN_wine_get_version) KernelBaseProxy::GetProcAddress_()(ntdll, "wine_get_version");

    // Workaround for the ntdll-Hide_Wine_Exports patch
    if (!pWineGetVersion && KernelBaseProxy::GetProcAddress_()(ntdll, "wine_server_call") != nullptr)
        pWineGetVersion = (PFN_wine_get_version) ManualGetProcAddress(ntdll, "wine_get_version");

    if (pWineGetVersion)
    {
        LOG_INFO("Running on Wine {0}!", pWineGetVersion());
        return true;
    }

    LOG_WARN("Wine not detected");
    return false;
}

UINT customD3D12SDKVersion = 615;

static void RunAgilityUpgrade(HMODULE dx12Module)
{
    typedef HRESULT (*PFN_IsDeveloperModeEnabled)(BOOL* isEnabled);
    PFN_IsDeveloperModeEnabled o_IsDeveloperModeEnabled =
        (PFN_IsDeveloperModeEnabled) GetProcAddress(GetModuleHandle(L"kernelbase.dll"), "IsDeveloperModeEnabled");

    if (o_IsDeveloperModeEnabled == nullptr)
    {
        LOG_ERROR("Failed to get IsDeveloperModeEnabled function address");
        return;
    }

    auto hk_IsDeveloperModeEnabled = [](BOOL* isEnabled) -> HRESULT
    {
        *isEnabled = TRUE;
        return S_OK;
    };

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&) o_IsDeveloperModeEnabled, static_cast<HRESULT (*)(BOOL*)>(hk_IsDeveloperModeEnabled));
    auto detourResult = DetourTransactionCommit();

    if (detourResult != NO_ERROR)
    {
        LOG_ERROR("Failed to attach detour: {:X}", detourResult);
        return;
    }

    if (Config::Instance()->FsrAgilitySDKUpgrade.value_or_default())
    {
        Microsoft::WRL::ComPtr<ID3D12SDKConfiguration> sdkConfig;
        auto hr = D3D12GetInterface(CLSID_D3D12SDKConfiguration, IID_PPV_ARGS(&sdkConfig));

        if (SUCCEEDED(hr))
        {
            static bool pathSet = false;
            static std::string path; // narrow string

            if (!pathSet)
            {
                std::wstring widePath = Config::Instance()->MainDllPath.value();
                widePath = std::filesystem::relative(widePath, Util::ExePath().parent_path());
                widePath = widePath + L"\\D3D12_OptiScaler\\";

                // Properly convert wstring → string
                int size = WideCharToMultiByte(CP_UTF8, 0, widePath.c_str(), -1, nullptr, 0, nullptr, nullptr);
                path.resize(size - 1);
                WideCharToMultiByte(CP_UTF8, 0, widePath.c_str(), -1, path.data(), size, nullptr, nullptr);

                pathSet = true;
            }

            hr = sdkConfig->SetSDKVersion(customD3D12SDKVersion, reinterpret_cast<LPCSTR>(path.c_str()));
            if (FAILED(hr))
            {
                LOG_ERROR("Failed to upgrade Agility SDK: {0}", hr);
            }
            else
            {
                LOG_INFO("Agility SDK upgraded successfully");
            }
        }
        else
        {
            LOG_ERROR("Failed to get D3D12 SDK Configuration interface: {0}", hr);
        }
    }

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(&(PVOID&) o_IsDeveloperModeEnabled, static_cast<HRESULT (*)(BOOL*)>(hk_IsDeveloperModeEnabled));
    detourResult = DetourTransactionCommit();

    if (detourResult != NO_ERROR)
    {
        LOG_ERROR("Failed to detach detour: {:X}", detourResult);
        return;
    }
}

void LoadAsiPlugins()
{
    std::filesystem::path pluginPath(Config::Instance()->PluginPath.value_or(L"plugins"));
    auto folderPath = pluginPath.wstring();

    LOG_DEBUG(L"Checking {} for *.asi", folderPath);

    if (!std::filesystem::exists(pluginPath))
        return;

    for (const auto& entry : std::filesystem::directory_iterator(folderPath))
    {
        if (!entry.is_regular_file())
            continue;

        std::wstring ext = entry.path().extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return std::towlower(c); });

        if (ext == L".asi")
        {
            std::wstring fileName = entry.path().filename().wstring();
            std::transform(fileName.begin(), fileName.end(), fileName.begin(),
                           [](wchar_t c) { return std::towlower(c); });

            if (fileName == L"mfgunlocker.asi")
            {
                LOG_INFO("Built-in FG path selected; skipping MFGUnlocker.asi");
                continue;
            }

            HMODULE hMod = nullptr;

            if (fileName.rfind(L"-loadlate") != std::wstring::npos)
                _lateLoadingEntries.push_back(entry);
            else
                hMod = NtdllProxy::LoadLibraryExW_Ldr(entry.path().c_str(), NULL, 0);

            if (hMod != nullptr)
            {
                LOG_INFO(L"Loaded: {}", entry.path().wstring());
                _asiHandles.push_back(hMod);

                auto init = (PFN_InitializeASI) KernelBaseProxy::GetProcAddress_()(hMod, "InitializeASI");
                auto patchResult = (PFN_PatchResult) KernelBaseProxy::GetProcAddress_()(hMod, "PatchResult");

                if (init != nullptr)
                    init();

                if (patchResult != nullptr)
                {
                    auto pr = patchResult();

                    if (pr)
                    {
                        LOG_INFO("Game patching is successful");
                        State::Instance().isOptiPatcherSucceed = true;

                        LOG_INFO("Disabling spoofing");

                        if (!Config::Instance()->DxgiSpoofing.has_value())
                            Config::Instance()->DxgiSpoofing.set_volatile_value(false);

                        if (!Config::Instance()->VulkanSpoofing.has_value() ||
                            !Config::Instance()->VulkanSpoofing.value_for_config())
                        {
                            Config::Instance()->VulkanSpoofing.set_volatile_value(false);
                        }

                        if (!Config::Instance()->VulkanExtensionSpoofing.has_value())
                            Config::Instance()->VulkanExtensionSpoofing.set_volatile_value(false);
                    }
                }
            }
            else
            {
                DWORD err = GetLastError();
                LOG_ERROR(L"Failed to load: {}, error {:X}", entry.path().wstring(), err);
            }

            if (_lateLoadingEntries.size() > 0)
            {
                std::thread(
                    []()
                    {
                        try
                        {

                            auto delay = Config::Instance()->LateAsiPluginsDelay.value_or_default();
                            LOG_INFO("Waiting {} seconds before loading late-load plugins...", delay);
                            std::this_thread::sleep_for(std::chrono::seconds(delay));

                            for (const auto& entry : _lateLoadingEntries)
                            {
                                HMODULE hMod = NtdllProxy::LoadLibraryExW_Ldr(entry.path().c_str(), NULL, 0);

                                if (hMod != nullptr)
                                {
                                    LOG_INFO(L"Loaded late: {}", entry.path().wstring());
                                    _asiHandles.push_back(hMod);

                                    auto init =
                                        (PFN_InitializeASI) KernelBaseProxy::GetProcAddress_()(hMod, "InitializeASI");

                                    auto patchResult =
                                        (PFN_PatchResult) KernelBaseProxy::GetProcAddress_()(hMod, "PatchResult");

                                    if (init != nullptr)
                                        init();

                                    if (patchResult != nullptr)
                                    {
                                        auto pr = patchResult();
                                        if (pr)
                                        {
                                            LOG_INFO("Game patching is successful");
                                            State::Instance().isOptiPatcherSucceed = true;
                                            LOG_INFO("Disabling spoofing");

                                            if (!Config::Instance()->DxgiSpoofing.has_value())
                                                Config::Instance()->DxgiSpoofing.set_volatile_value(false);

                                            if (!Config::Instance()->VulkanSpoofing.has_value() ||
                                                !Config::Instance()->VulkanSpoofing.value_for_config())
                                            {
                                                Config::Instance()->VulkanSpoofing.set_volatile_value(false);
                                            }

                                            if (!Config::Instance()->VulkanExtensionSpoofing.has_value())
                                                Config::Instance()->VulkanExtensionSpoofing.set_volatile_value(false);
                                        }
                                    }
                                }
                                else
                                {
                                    DWORD err = GetLastError();
                                    LOG_ERROR(L"Failed to load: {}, error {:X}", entry.path().wstring(), err);
                                }
                            }
                        }
                        catch (...)
                        {
                            LOG_ERROR("Exception occurred while loading late-load plugins");
                        }
                    })
                    .detach();
            }
        }
    }
}

// nvapi64.dll is loaded only because the startup check below (isNvidiaViaNvapi) keeps it loaded after NvAPI_Initialize.
// CheckWorkingMode must not take that for the game's own nvapi: it runs inside DLL_PROCESS_ATTACH, where GPU detection
// fails, and LoadNvApi would then pick fakenvapi as the main nvapi on an NVIDIA card. The game's own load of nvapi goes
// through the LoadLibrary hooks later, when the GPU is known.
static bool nvapiHeldByStartupCheck = false;

static void CheckWorkingMode()
{
    // Finish this real startup installer batch before starting optional signed
    // image setup. No waiting/signature I/O occurs under this startup scope.
    GenshinNativeQuery::StartupInstallScope nativeQueryStartup;
    if (!_passThruMode)
        LOG_FUNC();

    bool modeFound = false;
    std::string filename = wstring_to_string(Util::DllPath().filename().wstring()); // .string() can crash
    std::string lCaseFilename(filename);
    std::filesystem::path pluginPath(Config::Instance()->PluginPath.value_or(L"plugins"));
    auto optiDllPath = std::filesystem::path(Config::Instance()->MainDllPath.value());

    for (size_t i = 0; i < lCaseFilename.size(); i++)
        lCaseFilename[i] = std::tolower(lCaseFilename[i]);

    do
    {
        if (!_passThruMode && Config::Instance()->EarlyHooking.value_or_default())
        {
            NtdllHooks::Hook();
            KernelHooks::Hook();
            KernelHooks::HookBase();
        }

        // version.dll
        if (lCaseFilename == "version.dll")
        {
            do
            {
                auto pluginFilePath = pluginPath / L"version.dll";
                originalModule = NtdllProxy::LoadLibraryExW_Ldr(pluginFilePath.wstring().c_str(), NULL, 0);

                if (originalModule != nullptr)
                {
                    if (!_passThruMode)
                        LOG_INFO("OptiScaler working as version.dll, original dll loaded from plugin folder");

                    break;
                }

                originalModule = NtdllProxy::LoadLibraryExW_Ldr(L"version-original.dll", NULL, 0);

                if (originalModule != nullptr)
                {
                    if (!_passThruMode)
                        LOG_INFO("OptiScaler working as version.dll, version-original.dll loaded");

                    break;
                }

                originalModule = NtdllProxy::LoadLibraryExW_Ldr(L"version.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);

                if (originalModule != nullptr && !_passThruMode)
                    LOG_INFO("OptiScaler working as version.dll, system dll loaded");

            } while (false);

            if (originalModule != nullptr)
            {
                dllNames.push_back("version.dll");
                dllNames.push_back("version");
                dllNamesW.push_back(L"version.dll");
                dllNamesW.push_back(L"version");

                shared.LoadOriginalLibrary(originalModule);
                version.LoadOriginalLibrary(originalModule);

                modeFound = true;
            }
            else
            {
                if (!_passThruMode)
                    LOG_ERROR("OptiScaler can't find original version.dll!");
            }

            break;
        }

        // winmm.dll
        if (lCaseFilename == "winmm.dll")
        {
            do
            {
                auto pluginFilePath = pluginPath / L"winmm.dll";
                originalModule = NtdllProxy::LoadLibraryExW_Ldr(pluginFilePath.wstring().c_str(), NULL, 0);

                if (originalModule != nullptr)
                {
                    if (!_passThruMode)
                        LOG_INFO("OptiScaler working as winmm.dll, original dll loaded from plugin folder");

                    break;
                }

                originalModule = NtdllProxy::LoadLibraryExW_Ldr(L"winmm-original.dll", NULL, 0);

                if (originalModule != nullptr)
                {
                    if (!_passThruMode)
                        LOG_INFO("OptiScaler working as winmm.dll, winmm-original.dll loaded");

                    break;
                }

                originalModule = NtdllProxy::LoadLibraryExW_Ldr(L"winmm.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);

                if (originalModule != nullptr && !_passThruMode)
                    LOG_INFO("OptiScaler working as winmm.dll, system dll loaded");

            } while (false);

            if (originalModule != nullptr)
            {
                dllNames.push_back("winmm.dll");
                dllNames.push_back("winmm");
                dllNamesW.push_back(L"winmm.dll");
                dllNamesW.push_back(L"winmm");

                shared.LoadOriginalLibrary(originalModule);
                winmm.LoadOriginalLibrary(originalModule);
                modeFound = true;
            }
            else
            {
                if (!_passThruMode)
                    LOG_ERROR("OptiScaler can't find original winmm.dll!");
            }

            break;
        }

        // wininet.dll
        if (lCaseFilename == "wininet.dll")
        {
            do
            {
                auto pluginFilePath = pluginPath / L"wininet.dll";
                originalModule = NtdllProxy::LoadLibraryExW_Ldr(pluginFilePath.wstring().c_str(), NULL, 0);

                if (originalModule != nullptr)
                {
                    if (!_passThruMode)
                        LOG_INFO("OptiScaler working as wininet.dll, original dll loaded from plugin folder");

                    break;
                }

                originalModule = NtdllProxy::LoadLibraryExW_Ldr(L"wininet-original.dll", NULL, 0);

                if (originalModule != nullptr)
                {
                    if (!_passThruMode)
                        LOG_INFO("OptiScaler working as wininet.dll, wininet-original.dll loaded");

                    break;
                }

                originalModule = NtdllProxy::LoadLibraryExW_Ldr(L"wininet.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);

                if (originalModule != nullptr && !_passThruMode)
                    LOG_INFO("OptiScaler working as wininet.dll, system dll loaded");

            } while (false);

            if (originalModule != nullptr)
            {
                dllNames.push_back("wininet.dll");
                dllNames.push_back("wininet");
                dllNamesW.push_back(L"wininet.dll");
                dllNamesW.push_back(L"wininet");

                shared.LoadOriginalLibrary(originalModule);
                wininet.LoadOriginalLibrary(originalModule);
                modeFound = true;
            }
            else
            {
                if (!_passThruMode)
                    LOG_ERROR("OptiScaler can't find original wininet.dll!");
            }

            break;
        }

        // dbghelp.dll
        if (lCaseFilename == "dbghelp.dll")
        {
            do
            {
                auto pluginFilePath = pluginPath / L"dbghelp.dll";
                originalModule = NtdllProxy::LoadLibraryExW_Ldr(pluginFilePath.wstring().c_str(), NULL, 0);

                if (originalModule != nullptr)
                {
                    if (!_passThruMode)
                        LOG_INFO("OptiScaler working as dbghelp.dll, original dll loaded from plugin folder");

                    break;
                }

                originalModule = NtdllProxy::LoadLibraryExW_Ldr(L"dbghelp-original.dll", NULL, 0);

                if (originalModule != nullptr)
                {
                    if (!_passThruMode)
                        LOG_INFO("OptiScaler working as dbghelp.dll, dbghelp-original.dll loaded");

                    break;
                }

                originalModule = NtdllProxy::LoadLibraryExW_Ldr(L"dbghelp.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);

                if (originalModule != nullptr && !_passThruMode)
                    LOG_INFO("OptiScaler working as dbghelp.dll, system dll loaded");

            } while (false);

            if (originalModule != nullptr)
            {
                dllNames.push_back("dbghelp.dll");
                dllNames.push_back("dbghelp");
                dllNamesW.push_back(L"dbghelp.dll");
                dllNamesW.push_back(L"dbghelp");

                shared.LoadOriginalLibrary(originalModule);
                dbghelp.LoadOriginalLibrary(originalModule);
                modeFound = true;
            }
            else
            {
                if (!_passThruMode)
                    LOG_ERROR("OptiScaler can't find original dbghelp.dll!");
            }

            break;
        }

        // optiscaler.dll
        if (lCaseFilename == "optiscaler.dll")
        {
            if (!_passThruMode)
                LOG_INFO("OptiScaler working as OptiScaler.dll");

            // quick hack for testing
            originalModule = dllModule;

            dllNames.push_back("optiscaler.dll");
            dllNames.push_back("optiscaler");
            dllNamesW.push_back(L"optiscaler.dll");
            dllNamesW.push_back(L"optiscaler");

            modeFound = true;
            break;
        }

        // optiscaler.asi
        if (lCaseFilename == "optiscaler.asi")
        {
            if (!_passThruMode)
                LOG_INFO("OptiScaler working as OptiScaler.asi");

            // quick hack for testing
            originalModule = dllModule;

            dllNames.push_back("optiscaler.asi");
            dllNames.push_back("optiscaler");
            dllNamesW.push_back(L"optiscaler.asi");
            dllNamesW.push_back(L"optiscaler");

            modeFound = true;
            break;
        }

        // winhttp.dll
        if (lCaseFilename == "winhttp.dll")
        {
            do
            {
                auto pluginFilePath = pluginPath / L"winhttp.dll";
                originalModule = NtdllProxy::LoadLibraryExW_Ldr(pluginFilePath.wstring().c_str(), NULL, 0);

                if (originalModule != nullptr)
                {
                    if (!_passThruMode)
                        LOG_INFO("OptiScaler working as winhttp.dll, original dll loaded from plugin folder");

                    break;
                }

                originalModule = NtdllProxy::LoadLibraryExW_Ldr(L"winhttp-original.dll", NULL, 0);

                if (originalModule != nullptr)
                {
                    if (!_passThruMode)
                        LOG_INFO("OptiScaler working as winhttp.dll, winhttp-original.dll loaded");

                    break;
                }

                originalModule = NtdllProxy::LoadLibraryExW_Ldr(L"winhttp.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);

                if (originalModule != nullptr && !_passThruMode)
                    LOG_INFO("OptiScaler working as winhttp.dll, system dll loaded");

            } while (false);

            if (originalModule != nullptr)
            {
                dllNames.push_back("winhttp.dll");
                dllNames.push_back("winhttp");
                dllNamesW.push_back(L"winhttp.dll");
                dllNamesW.push_back(L"winhttp");

                shared.LoadOriginalLibrary(originalModule);
                winhttp.LoadOriginalLibrary(originalModule);
                modeFound = true;
            }
            else
            {
                if (!_passThruMode)
                    LOG_ERROR("OptiScaler can't find original winhttp.dll!");
            }

            break;
        }

        // dxgi.dll
        if (lCaseFilename == "dxgi.dll")
        {
            do
            {
                auto pluginFilePath = pluginPath / L"dxgi.dll";
                originalModule = NtdllProxy::LoadLibraryExW_Ldr(pluginFilePath.wstring().c_str(), NULL, 0);

                if (originalModule != nullptr)
                {
                    if (!_passThruMode)
                        LOG_INFO("OptiScaler working as dxgi.dll, original dll loaded from plugin folder");

                    break;
                }

                originalModule = NtdllProxy::LoadLibraryExW_Ldr(L"dxgi-original.dll", NULL, 0);

                if (originalModule != nullptr)
                {
                    if (!_passThruMode)
                        LOG_INFO("OptiScaler working as dxgi.dll, dxgi-original.dll loaded");

                    break;
                }

                originalModule = NtdllProxy::LoadLibraryExW_Ldr(L"dxgi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);

                if (originalModule != nullptr && !_passThruMode)
                    LOG_INFO("OptiScaler working as dxgi.dll, system dll loaded");

            } while (false);

            if (originalModule != nullptr)
            {
                dllNames.push_back("dxgi.dll");
                dllNames.push_back("dxgi");
                dllNamesW.push_back(L"dxgi.dll");
                dllNamesW.push_back(L"dxgi");

                DxgiProxy::Init(originalModule);
                dxgi.LoadOriginalLibrary(originalModule);

                State::Instance().workingMode = WorkingMode::Dxgi;
                modeFound = true;
            }
            else
            {
                if (!_passThruMode)
                    LOG_ERROR("OptiScaler can't find original dxgi.dll!");
            }

            break;
        }

        // d3d12.dll
        if (lCaseFilename == "d3d12.dll")
        {
            do
            {
                // Moved here to cover agility sdk
                if (!_passThruMode)
                {
                    NtdllHooks::Hook();
                    KernelHooks::HookBase();
                }

                auto pluginFilePath = pluginPath / L"d3d12.dll";
                originalModule = NtdllProxy::LoadLibraryExW_Ldr(pluginFilePath.wstring().c_str(), NULL, 0);
                if (originalModule != nullptr)
                {
                    if (!_passThruMode)
                        LOG_INFO("OptiScaler working as d3d12.dll, original dll loaded from plugin folder");

                    break;
                }

                originalModule = NtdllProxy::LoadLibraryExW_Ldr(L"d3d12-original.dll", NULL, 0);
                if (originalModule != nullptr)
                {
                    if (!_passThruMode)
                        LOG_INFO("OptiScaler working as d3d12.dll, d3d12-original.dll loaded");

                    break;
                }

                originalModule = NtdllProxy::LoadLibraryExW_Ldr(L"d3d12.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);

                if (originalModule != nullptr && !_passThruMode)
                    LOG_INFO("OptiScaler working as d3d12.dll, system dll loaded");

            } while (false);

            if (originalModule != nullptr)
            {
                dllNames.push_back("d3d12.dll");
                dllNames.push_back("d3d12");
                dllNamesW.push_back(L"d3d12.dll");
                dllNamesW.push_back(L"d3d12");

                D3d12Proxy::Init(originalModule);
                d3d12.LoadOriginalLibrary(originalModule);

                State::Instance().workingMode = WorkingMode::D3d12;

                modeFound = true;
            }
            else
            {
                if (!_passThruMode)
                    LOG_ERROR("OptiScaler can't find original d3d12.dll!");
            }

            break;
        }

    } while (false);

    // Work as a dummy dll
    if (_passThruMode)
        return;

    if (!modeFound)
    {
        LOG_ERROR("Unsupported dll name: {0}", filename);
        return;
    }

    Config::Instance()->CheckUpscalerFiles();

    // Intel Extension Framework
    if (Config::Instance()->UESpoofIntelAtomics64.value_or_default())
    {
        HMODULE igdext = NtdllProxy::LoadLibraryExW_Ldr(L"igdext64.dll", NULL, 0);

        if (igdext == nullptr)
        {
            auto paths = Util::GetDriverStore();

            for (const auto& [luid, path] : paths)
            {
                auto dllPath = path / L"igdext64.dll";
                LOG_DEBUG("Trying to load: {}", wstring_to_string(dllPath.c_str()));
                igdext = NtdllProxy::LoadLibraryExW_Ldr(dllPath.c_str(), NULL, 0);

                if (igdext != nullptr)
                {
                    LOG_INFO(L"igdext64.dll loaded from {}", dllPath.wstring());
                    break;
                }
            }
        }
        else
        {
            LOG_INFO("igdext64.dll loaded from game folder");
        }

        if (igdext != nullptr)
            IGDExtProxy::Init(igdext);
        else
            LOG_ERROR("Failed to load igdext64.dll");
    }

    // Enroll Genshin creation factories before exposing DXGI hooks. This
    // remains installation only; signed-image preparation stays on the worker.
    const auto installD3D11Factories=[](){
        d3d11Module = GetDllNameWModule(&dx11NamesW);
        if (Config::Instance()->OverlayMenu.value_or_default() && d3d11Module != nullptr)
        {
            LOG_DEBUG("d3d11.dll already in memory");
            D3D11Hooks::Hook(d3d11Module);
        }
    };
    const bool earlyGenshinFactories=F6InputPolicy::FsrBridgeEnabled && GenshinFsr::IsGenshinExecutable(Util::ExePath().wstring()) &&
        Config::Instance()->AcquireGenshinControls().Active();
    if(earlyGenshinFactories)installD3D11Factories();

    // DXGI
    if (DxgiProxy::Module() == nullptr)
    {
        LOG_DEBUG("Check for dxgi");
        HMODULE dxgiModule = nullptr;
        dxgiModule = GetDllNameWModule(&dxgiNamesW);
        if (dxgiModule != nullptr)
        {
            LOG_DEBUG("dxgi.dll already in memory");

            DxgiProxy::Init(dxgiModule);
            DxgiHooks::Hook();
        }
    }
    else
    {
        LOG_DEBUG("dxgi.dll already in memory");
        DxgiHooks::Hook();
    }

    // DirectX 12
    if (D3d12Proxy::Module() == nullptr)
    {
        // Moved here to cover agility sdk
        KernelHooks::HookBase();
        NtdllHooks::Hook();

        LOG_DEBUG("Check for d3d12");
        HMODULE d3d12Module = nullptr;
        d3d12Module = GetDllNameWModule(&dx12NamesW);
        if (Config::Instance()->OverlayMenu.value_or_default() && d3d12Module != nullptr)
        {
            LOG_DEBUG("d3d12.dll already in memory");
            D3d12Proxy::Init(d3d12Module);
            D3D12Hooks::Hook();
        }
    }
    else
    {
        LOG_DEBUG("d3d12.dll already in memory");
        D3D12Hooks::Hook();
    }

    if (D3d12Proxy::Module() == nullptr && State::Instance().gameQuirks & GameQuirk::LoadD3D12Manually)
    {
        LOG_DEBUG("Loading d3d12.dll manually");
        D3d12Proxy::Init();
        D3D12Hooks::Hook();
    }

    d3d12AgilityModule = GetDllNameWModule(&dx12agilityNamesW);
    if (d3d12AgilityModule != nullptr)
    {
        LOG_DEBUG("D3D12Core.dll already in memory");
        D3D12Hooks::HookAgility(d3d12AgilityModule);
    }

    if (d3d12AgilityModule == nullptr && State::Instance().gameQuirks & GameQuirk::LoadD3D12Manually)
    {
        auto path = Util::ExePath().parent_path() / L"D3D12" / L"D3D12Core.dll";
        d3d12AgilityModule = NtdllProxy::LoadLibraryExW_Ldr(path.c_str(), NULL, 0);

        if (d3d12AgilityModule == nullptr && Config::Instance()->FsrAgilitySDKUpgrade.value_or_default())
        {
            path = optiDllPath / L"D3D12_OptiScaler" / L"D3D12Core.dll";
            d3d12AgilityModule = NtdllProxy::LoadLibraryExW_Ldr(path.c_str(), NULL, 0);
        }

        if (d3d12AgilityModule == nullptr)
        {
            d3d12AgilityModule = NtdllProxy::LoadLibraryExW_Ldr(L"D3D12Core.dll", NULL, 0);
        }

        if (d3d12AgilityModule != nullptr)
        {
            LOG_DEBUG("D3D12Core.dll loaded");
            D3D12Hooks::HookAgility(d3d12AgilityModule);
        }
    }

    // Other games retain their original DXGI/D3D12/D3D11 install order.
    if(!earlyGenshinFactories)installD3D11Factories();

    // Vulkan
    if (State::Instance().isRunningOnLinux || State::Instance().gameQuirks & GameQuirk::LoadVulkanManually)
    {
        vulkanModule = NtdllProxy::LoadLibraryExW_Ldr(L"vulkan-1.dll", NULL, 0);
        LOG_DEBUG("Loading vulkan-1.dll for Linux, result: {:X}", (size_t) vulkanModule);
    }
    else
    {
        vulkanModule = GetDllNameWModule(&vkNamesW);
    }

    if (vulkanModule != nullptr)
    {
        LOG_DEBUG("Hooking vulkan-1.dll");
        VulkanHooks::Hook(vulkanModule);
    }

    // NVAPI
    // Doesn't seem to like GetModuleHandle for some reason, so call our load to make sure
    if (!nvapiHeldByStartupCheck && GetDllNameWModule(&nvapiNamesW) != nullptr)
    {
        // This hooks nvapi as well when possible
        auto nvapi64 = LibraryLoadHooks::LoadNvApi();
    }

    // GDI32
    hookGdi32();

    // Wintrust
    hookWintrust();

    // Crypt32
    hookCrypt32();

    // Advapi32
    if (Config::Instance()->DxgiSpoofing.value_or_default() ||
        Config::Instance()->StreamlineSpoofing.value_or_default())
    {
        hookAdvapi32();
    }

    // User32
    if (Config::Instance()->SpoofUser32.value_or_default())
    {
        User32Spoofing::Hook();
    }

    // hook streamline right away if it's already loaded
    HMODULE slModule = nullptr;
    slModule = GetDllNameWModule(&slInterposerNamesW);
    if (slModule != nullptr)
    {
        LOG_DEBUG("sl.interposer.dll already in memory");
        StreamlineHooks::hookInterposer(slModule);
        slInterposerModule = slModule;
    }

    HMODULE slDlss = nullptr;
    slDlss = GetDllNameWModule(&slDlssNamesW);
    if (slDlss != nullptr)
    {
        State::Instance().postCodes |= PostCode::SlPluginsAlreadyInMemory;
        LOG_WARN("sl.dlss.dll already in memory");
        StreamlineHooks::hookDlss(slDlss);
    }

    HMODULE slDlssg = nullptr;
    slDlssg = GetDllNameWModule(&slDlssgNamesW);
    if (slDlssg != nullptr)
    {
        State::Instance().postCodes |= PostCode::SlPluginsAlreadyInMemory;
        LOG_WARN("sl.dlss_g.dll already in memory");
        StreamlineHooks::hookDlssg(slDlssg);
    }

    HMODULE slReflex = nullptr;
    slReflex = GetDllNameWModule(&slReflexNamesW);
    if (slReflex != nullptr)
    {
        State::Instance().postCodes |= PostCode::SlPluginsAlreadyInMemory;
        LOG_WARN("sl.reflex.dll already in memory");
        StreamlineHooks::hookReflex(slReflex);
    }

    HMODULE slPcl = nullptr;
    slPcl = GetDllNameWModule(&slPclNamesW);
    if (slPcl != nullptr)
    {
        State::Instance().postCodes |= PostCode::SlPluginsAlreadyInMemory;
        LOG_WARN("sl.pcl.dll already in memory");
        StreamlineHooks::hookPcl(slPcl);
    }

    HMODULE slCommon = nullptr;
    slCommon = GetDllNameWModule(&slCommonNamesW);
    if (slCommon != nullptr)
    {
        State::Instance().postCodes |= PostCode::SlPluginsAlreadyInMemory;
        LOG_WARN("sl.common.dll already in memory");
        StreamlineHooks::hookCommon(slCommon);
    }

    // XeSS
    HMODULE xessModule = nullptr;
    xessModule = GetDllNameWModule(&xessNamesW);
    if (xessModule != nullptr)
    {
        LOG_DEBUG("libxess.dll already in memory");
        XeSSProxy::InitXeSS(xessModule);
    }

    HMODULE xessDx11Module = nullptr;
    xessDx11Module = GetDllNameWModule(&xessDx11NamesW);
    if (xessDx11Module != nullptr)
    {
        LOG_DEBUG("libxess_dx11.dll already in memory");
        XeSSProxy::InitXeSSDx11(xessDx11Module);
    }

    // NVNGX
    HMODULE nvngxModule = nullptr;
    nvngxModule = GetDllNameWModule(&nvngxNamesW);
    if (nvngxModule != nullptr)
    {
        LOG_DEBUG("nvngx.dll already in memory");
        // Resolving the whole native table also installs ZZZ's DX12 input
        // router, including entry points already cached by game Streamline.
        NVNGXProxy::InitNVNGX(nvngxModule);
    }

    // FFX Dx12
    HMODULE ffxDx12Module = nullptr;
    ffxDx12Module = GetDllNameWModule(&ffxDx12NamesW);
    if (ffxDx12Module != nullptr)
    {
        LOG_DEBUG("amd_fidelityfx_dx12.dll already in memory");
        FfxApiProxy::InitFfxDx12(ffxDx12Module);
    }

    HMODULE ffxDx12SRModule = nullptr;
    ffxDx12SRModule = GetDllNameWModule(&ffxDx12UpscalerNamesW);
    if (ffxDx12SRModule != nullptr)
    {
        LOG_DEBUG("amd_fidelityfx_upscaler_dx12.dll already in memory");
        FfxApiProxy::InitFfxDx12_SR(ffxDx12SRModule);
    }

    HMODULE ffxDx12FGModule = nullptr;
    ffxDx12FGModule = GetDllNameWModule(&ffxDx12FGNamesW);
    if (ffxDx12FGModule != nullptr)
    {
        LOG_DEBUG("amd_fidelityfx_framegeneration_dx12.dll already in memory");
        FfxApiProxy::InitFfxDx12_FG(ffxDx12FGModule);
    }

    HMODULE ffxDx12DenoiserModule = nullptr;
    ffxDx12DenoiserModule = GetDllNameWModule(&ffxDx12DenoiserNamesW);
    if (ffxDx12DenoiserModule != nullptr)
    {
        LOG_DEBUG("amd_fidelityfx_denoiser_dx12.dll already in memory");
        FfxApiProxy::InitFfxDx12_Denoiser(ffxDx12DenoiserModule);
    }

    HMODULE ffxDx12RadianceModule = nullptr;
    ffxDx12RadianceModule = GetDllNameWModule(&ffxDx12RadianceNamesW);
    if (ffxDx12RadianceModule != nullptr)
    {
        LOG_DEBUG("amd_fidelityfx_radiancecache_dx12.dll already in memory");
        FfxApiProxy::InitFfxDx12_Radiance(ffxDx12RadianceModule);
    }

    // FFX Vulkan
    HMODULE ffxVkModule = nullptr;
    ffxVkModule = GetDllNameWModule(&ffxVkNamesW);
    if (ffxVkModule != nullptr)
    {
        LOG_DEBUG("amd_fidelityfx_vk.dll already in memory");
        FfxApiProxy::InitFfxVk(ffxVkModule);
    }

    // Hook kernel32 methods
    if (!Config::Instance()->EarlyHooking.value_or_default())
    {
        NtdllHooks::Hook();
        KernelHooks::Hook();
    }

    // For Agility SDK Upgrade
    if (Config::Instance()->FsrAgilitySDKUpgrade.value_or_default())
    {
        RunAgilityUpgrade(GetDllNameWModule(&dx12NamesW));
    }

    // SpecialK
    if (skModule == nullptr && Config::Instance()->LoadSpecialK.value_or_default())
    {
        auto skFile = Util::ExePath().parent_path() / L"SpecialK64.dll";
        SetEnvironmentVariableW(L"RESHADE_DISABLE_GRAPHICS_HOOK", L"1");

        State::EnableServeOriginal(200);
        skModule = NtdllProxy::LoadLibraryExW_Ldr(skFile.c_str(), NULL, 0);
        State::DisableServeOriginal(200);

        LOG_INFO("Loading SpecialK64.dll, result: {0:X}", (UINT64) skModule);
    }

    // ReShade
    // Do not load Reshade here is Luma is active and we will create D3D12 device for it
    // We will load Reshade after D3D12 device creation in that case
    if (reshadeModule == nullptr && Config::Instance()->LoadReShade.value_or_default() &&
        !Config::Instance()->CreateD3D12DeviceForLuma.value_or_default())
    {
        auto rsFile = Util::ExePath().parent_path() / L"ReShade64.dll";
        SetEnvironmentVariableW(L"RESHADE_DISABLE_LOADING_CHECK", L"1");

        if (skModule != nullptr)
            SetEnvironmentVariableW(L"RESHADE_DISABLE_GRAPHICS_HOOK", L"1");

        State::EnableServeOriginal(201);
        reshadeModule = NtdllProxy::LoadLibraryExW_Ldr(rsFile.c_str(), NULL, 0);
        State::DisableServeOriginal(201);

        LOG_INFO("Loading ReShade64.dll, result: {0:X}", (size_t) reshadeModule);
    }

    // Version check
    if (Config::Instance()->CheckForUpdate.value_or_default())
        VersionCheck::Start();

    return;
}

static void printQuirks(flag_set<GameQuirk>& quirks)
{
    auto state = &State::Instance();
    std::vector<std::string> stringQuirks;

    if (quirks & GameQuirk::CyberpunkHudlessState)
        stringQuirks.push_back("Fixing DLSSG's hudless state in Cyberpunk");

    if (quirks & GameQuirk::FSRFGHudlessMismatchFixup)
        stringQuirks.push_back("FSR FG hudless mismatch fixup");

    if (quirks & GameQuirk::SkipFsr3Method)
        stringQuirks.push_back("Skipping first FSR 3 method");

    if (quirks & GameQuirk::LoadD3D12Manually)
        stringQuirks.push_back("Load d3d12.dll");

    if (quirks & GameQuirk::KernelBaseHooks)
        stringQuirks.push_back("Enable KernelBase hooks");

    if (quirks & GameQuirk::VulkanDLSSBarrierFixup)
        stringQuirks.push_back("Fix DLSS/DLSSG barriers on Vulkan");

    if (quirks & GameQuirk::ForceUnrealEngine)
        stringQuirks.push_back("Force detected engine as Unreal Engine");

    if (quirks & GameQuirk::DisableHudfix)
        stringQuirks.push_back("Disabling Hudfix due to known issues");

    if (quirks & GameQuirk::ForceAutoExposure)
        stringQuirks.push_back("Enabling AutoExposure");

    if (quirks & GameQuirk::DisableFFXInputs)
        stringQuirks.push_back("Disable FFX Inputs");

    if (quirks & GameQuirk::DisableFSR3Inputs)
        stringQuirks.push_back("Disable FSR 3.0 Inputs");

    if (quirks & GameQuirk::DisableFSR2Inputs)
        stringQuirks.push_back("Disable FSR 2.X Inputs");

    if (quirks & GameQuirk::DisableReactiveMasks)
        stringQuirks.push_back("Disable Reactive Masks");

    if (quirks & GameQuirk::RestoreComputeSigOnNonNvidia)
        stringQuirks.push_back("Enabling restore compute signature on AMD/Intel");

    if (quirks & GameQuirk::RestoreComputeSigOnNvidia)
        stringQuirks.push_back("Enabling restore compute signature on Nvidia");

    if (quirks & GameQuirk::ExtendedSigRestore)
        stringQuirks.push_back("Extended signatures restore");

    if (quirks & GameQuirk::DisableDxgiSpoofing)
        stringQuirks.push_back("Dxgi spoofing disabled by default");

    if (quirks & GameQuirk::DisableUseFsrInputValues)
        stringQuirks.push_back("Disable Use FSR Input Values");

    if (quirks & GameQuirk::DisableOptiXessPipelineCreation)
        stringQuirks.push_back("Disable custom pipeline creation for XeSS");

    if (quirks & GameQuirk::DontUseNTShared)
        stringQuirks.push_back("Don't use NTShared enabled");

    if (quirks & GameQuirk::DontUseUnrealColorBarriers)
        stringQuirks.push_back("Don't use color resource barrier fix for Unreal Engine games");

    if (quirks & GameQuirk::DontUseUnrealMVBarriers)
        stringQuirks.push_back("Don't use motion vector resource barrier fix for Unreal Engine games");

    if (quirks & GameQuirk::SkipFirst10Frames)
        stringQuirks.push_back("Skipping upscaling for first 10 frames");

    if (quirks & GameQuirk::NoFSRFGFirstSwapchain)
        stringQuirks.push_back("Skip turning the first swapchain created into an FSR swapchain");

    if (quirks & GameQuirk::FixSlSimulationMarkers)
        stringQuirks.push_back("Correct simulation start marker's frame id");

    if (quirks & GameQuirk::DisableVsyncOverride)
        stringQuirks.push_back("Don't use V-Sync overrides");

    if (quirks & GameQuirk::HitmanReflexHacks)
        stringQuirks.push_back("Hack for broken Hitman reflex");

    if (quirks & GameQuirk::SkipD3D11FeatureLevelElevation)
        stringQuirks.push_back("Skipping D3D11 feature level elevation, native FSR3.1 will be disabled!");

    if (quirks & GameQuirk::DontUseNtDllHooks)
        stringQuirks.push_back("Using kernel hooks instead of NTdll ones");

    if (quirks & GameQuirk::UseFSR2PatternMatching)
        stringQuirks.push_back("Use FSR2 pattern matching");

    if (quirks & GameQuirk::AlwaysCaptureFSRFGSwapchain)
        stringQuirks.push_back("Always capture FSR-FG swapchain");

    if (quirks & GameQuirk::AllowedFrameAhead2)
        stringQuirks.push_back("Allowed Frame Ahead: 2");

    if (quirks & GameQuirk::DisableXeFGChecks)
        stringQuirks.push_back("Skip pre init checks for XeFG");

    if (quirks & GameQuirk::CreateD3D12DeviceForLuma)
        stringQuirks.push_back("Create D3D12 device for Luma before loading Reshade");

    if (quirks & GameQuirk::LoadVulkanManually)
        stringQuirks.push_back("Load vulkan-1.dll");

    if (quirks & GameQuirk::UseFsr2Dx11Inputs)
        stringQuirks.push_back("Use FSR2 DX11 inputs");

    if (quirks & GameQuirk::UseFsr2VulkanInputs)
        stringQuirks.push_back("Use FSR2 Vulkan inputs");

    if (quirks & GameQuirk::ForceBorderlessWhenUsingXeFG)
        stringQuirks.push_back("Force Borderless when using XeFG");

    if (quirks & GameQuirk::OverrideVsyncWhenUsingXeFG)
        stringQuirks.push_back("Override Vsync when using XeFG");

    if (quirks & GameQuirk::ForceCreateD3D12Device)
        stringQuirks.push_back("Force create D3D12 device for w/Dx12");

    if (quirks & GameQuirk::DisableResizeSkip)
        stringQuirks.push_back("Disable Resize Skip");

    if (quirks & GameQuirk::SpoofRegistry)
        stringQuirks.push_back("Spoof Registry");

    if (quirks & GameQuirk::DisableFakenvapi)
        stringQuirks.push_back("Disable fakenvapi");

    if (quirks & GameQuirk::ForceDepthD32S8)
        stringQuirks.push_back("Force depth as D32S8");

    if (quirks & GameQuirk::DoNotPreserveFGSwapChain)
        stringQuirks.push_back("Don't Preserve FG Swapchain");

    if (quirks & GameQuirk::OldOverlayMenu)
        stringQuirks.push_back("Using old overlay (draws on upscaled image)");

    if (quirks & GameQuirk::PregmataFixDLSSModes)
        stringQuirks.push_back("Fix DLSS quality selection in Pragmata");

    if (quirks & GameQuirk::IgnoreValidUntilEvaluateForFG)
        stringQuirks.push_back("Ignore ValidUntilEvaluate resources for FG");

    if (quirks & GameQuirk::IgnoreTagsWithoutHudlessForFG)
        stringQuirks.push_back("Ignore tagging calls that lack Hudless resource for FG");

    if (quirks & GameQuirk::ForceFGRenderSizeMVs)
        stringQuirks.push_back("Force FG render size motion vectors");

    if (quirks & GameQuirk::CreateSLOnThe2ndDevice)
        stringQuirks.push_back("Create SL on the 2nd device");

    if (quirks & GameQuirk::Kcd2DlssgHdr10)
        stringQuirks.push_back("KCD2 native HDR10 for DLSSG");

    if (quirks & GameQuirk::XeFGBridgeResizeAsCreated)
        stringQuirks.push_back("XeFG bridge resizes as created");

    if (quirks & GameQuirk::REFrameworkCompat)
        stringQuirks.push_back("REFramework compatibility (XeFG)");

    state->detectedQuirks.append_range(stringQuirks);
    for (auto& stringQuirk : stringQuirks)
        spdlog::info("Quirk: {}", stringQuirk);

    return;
}

static void CheckQuirks(bool isNvidia)
{
    Util::GetExeInfo();

    LOG_INFO("Game's Exe: {0}", State::Instance().gameExe);
    LOG_INFO("Game Name: {0}", State::Instance().gameName);
    LOG_INFO("Game Version: {0}", State::Instance().gameVersion);
    LOG_INFO("Game Engine: {0}", magic_enum::enum_name(State::Instance().gameEngine));

#ifndef _DEBUG
    // Hash is very slow on Debug builds + we don't need to check our own hashes
    if (Config::Instance()->LogToFile.value_or_default() && Config::Instance()->LogLevel.value_or_default() == 0)
    {
        SHA1 checksum;
        std::ifstream file(Util::ExePath(), std::ios::binary);

        checksum.update(file);
        const std::string hash = checksum.final();

        LOG_TRACE("Game's Exe SHA1: {}", hash);
    }
#endif

    auto quirks = getQuirksForExe(State::Instance().gameExe);

    auto state = &State::Instance();

    // Apply config-level quirks
    if (quirks & GameQuirk::DisableHudfix && !Config::Instance()->FGDisableHUDFix.has_value() &&
        Config::Instance()->FGInput.value_or_default() == FGInput::Upscaler)
        Config::Instance()->FGDisableHUDFix.set_volatile_value(true);
    else
        quirks.reset(GameQuirk::DisableHudfix);

    if (quirks & GameQuirk::DisableFSR3Inputs && !Config::Instance()->EnableFsr3Inputs.has_value())
        Config::Instance()->EnableFsr3Inputs.set_volatile_value(false);
    else
        quirks.reset(GameQuirk::DisableFSR3Inputs);

    const bool disabledFsr2Inputs = (quirks & GameQuirk::DisableFSR2Inputs) &&
                                   Config::Instance()->ApplyFsr2InputDisableQuirk();
    if (!disabledFsr2Inputs)
        quirks.reset(GameQuirk::DisableFSR3Inputs);

    if (quirks & GameQuirk::DisableFFXInputs && !Config::Instance()->EnableFfxInputs.has_value())
        Config::Instance()->EnableFfxInputs.set_volatile_value(false);
    else
        quirks.reset(GameQuirk::DisableFFXInputs);

    if (quirks & GameQuirk::DisableDxgiSpoofing && !Config::Instance()->DxgiSpoofing.has_value())
        Config::Instance()->DxgiSpoofing.set_volatile_value(false);
    else
        quirks.reset(GameQuirk::DisableDxgiSpoofing);

    if (quirks & GameQuirk::RestoreComputeSigOnNonNvidia && !isNvidia &&
        !Config::Instance()->RestoreComputeSignature.has_value())
    {
        Config::Instance()->RestoreComputeSignature.set_volatile_value(true);
    }
    else
        quirks.reset(GameQuirk::RestoreComputeSigOnNonNvidia);

    if (quirks & GameQuirk::RestoreComputeSigOnNvidia && isNvidia &&
        !Config::Instance()->RestoreComputeSignature.has_value())
    {
        Config::Instance()->RestoreComputeSignature.set_volatile_value(true);
    }
    else
        quirks.reset(GameQuirk::RestoreComputeSigOnNvidia);

    if (quirks & GameQuirk::ExtendedSigRestore && !Config::Instance()->ExtendedStateRestore.has_value())
    {
        Config::Instance()->ExtendedStateRestore.set_volatile_value(true);
    }
    else
        quirks.reset(GameQuirk::ExtendedSigRestore);

    if (quirks & GameQuirk::DisableReactiveMasks)
        Config::Instance()->DisableReactiveMask.set_volatile_value(true);
    else
        quirks.reset(GameQuirk::DisableReactiveMasks);

    if (quirks & GameQuirk::ForceAutoExposure)
        Config::Instance()->AutoExposure.set_volatile_value(true);
    else
        quirks.reset(GameQuirk::ForceAutoExposure);

    if (quirks & GameQuirk::DisableUseFsrInputValues)
        Config::Instance()->FsrUseFsrInputValues.set_volatile_value(false);
    else
        quirks.reset(GameQuirk::DisableUseFsrInputValues);

    if (quirks & GameQuirk::EnableVulkanSpoofing && !isNvidia && !Config::Instance()->VulkanSpoofing.has_value())
    {
        Config::Instance()->VulkanSpoofing.set_volatile_value(true);
    }
    else
        quirks.reset(GameQuirk::EnableVulkanSpoofing);

    if (quirks & GameQuirk::EnableVulkanExtensionSpoofing && !isNvidia &&
        !Config::Instance()->VulkanExtensionSpoofing.has_value())
    {
        Config::Instance()->VulkanExtensionSpoofing.set_volatile_value(true);
    }
    else
        quirks.reset(GameQuirk::EnableVulkanExtensionSpoofing);

    if (quirks & GameQuirk::DisableOptiXessPipelineCreation && !Config::Instance()->CreateHeaps.has_value() &&
        !Config::Instance()->BuildPipelines.has_value())
    {
        Config::Instance()->CreateHeaps.set_volatile_value(false);
        Config::Instance()->BuildPipelines.set_volatile_value(false);
    }
    else
        quirks.reset(GameQuirk::DisableOptiXessPipelineCreation);

    if (quirks & GameQuirk::DontUseNTShared && !Config::Instance()->DontUseNTShared.has_value())
        Config::Instance()->DontUseNTShared.set_volatile_value(true);
    else
        quirks.reset(GameQuirk::DontUseNTShared);

    if (quirks & GameQuirk::DontUseUnrealColorBarriers && !Config::Instance()->ColorResourceBarrier.has_value())
        Config::Instance()->ColorResourceBarrier.set_volatile_value(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    else
        quirks.reset(GameQuirk::DontUseUnrealColorBarriers);

    if (quirks & GameQuirk::DontUseUnrealMVBarriers && !Config::Instance()->MVResourceBarrier.has_value())
        Config::Instance()->MVResourceBarrier.set_volatile_value(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    else
        quirks.reset(GameQuirk::DontUseUnrealMVBarriers);

    if (quirks & GameQuirk::SkipFirst10Frames && !Config::Instance()->SkipFirstFrames.has_value())
        Config::Instance()->SkipFirstFrames.set_volatile_value(10);
    else
        quirks.reset(GameQuirk::SkipFirst10Frames);

    if (quirks & GameQuirk::DisableVsyncOverride && !Config::Instance()->OverrideVsync.has_value())
        Config::Instance()->OverrideVsync.set_volatile_value(false);
    else
        quirks.reset(GameQuirk::DisableVsyncOverride);

    if (quirks & GameQuirk::DontUseNtDllHooks && !Config::Instance()->UseNtdllHooks.has_value())
        Config::Instance()->UseNtdllHooks.set_volatile_value(false);
    else
        quirks.reset(GameQuirk::DontUseNtDllHooks);

    if (quirks & GameQuirk::UseFSR2PatternMatching && !Config::Instance()->Fsr2Pattern.has_value())
        Config::Instance()->Fsr2Pattern.set_volatile_value(true);
    else
        quirks.reset(GameQuirk::UseFSR2PatternMatching);

    if (quirks & GameQuirk::AlwaysCaptureFSRFGSwapchain &&
        !Config::Instance()->FGAlwaysCaptureFSRFGSwapchain.has_value())
    {
        Config::Instance()->FGAlwaysCaptureFSRFGSwapchain.set_volatile_value(true);
    }
    else
        quirks.reset(GameQuirk::AlwaysCaptureFSRFGSwapchain);

    if (quirks & GameQuirk::AllowedFrameAhead2 && !Config::Instance()->FGAllowedFrameAhead.has_value())
        Config::Instance()->FGAllowedFrameAhead.set_volatile_value(2);
    else
        quirks.reset(GameQuirk::AllowedFrameAhead2);

    if (quirks & GameQuirk::DisableXeFGChecks && !Config::Instance()->FGXeFGIgnoreInitChecks.has_value())
        Config::Instance()->FGXeFGIgnoreInitChecks.set_volatile_value(true);
    else
        quirks.reset(GameQuirk::DisableXeFGChecks);

    if (quirks & GameQuirk::UseFsr2Dx11Inputs && !Config::Instance()->UseFsr2Dx11Inputs.has_value())
        Config::Instance()->UseFsr2Dx11Inputs.set_volatile_value(true);
    else
        quirks.reset(GameQuirk::UseFsr2Dx11Inputs);

    if (quirks & GameQuirk::UseFsr2VulkanInputs && !Config::Instance()->UseFsr2VulkanInputs.has_value())
        Config::Instance()->UseFsr2VulkanInputs.set_volatile_value(true);
    else
        quirks.reset(GameQuirk::UseFsr2VulkanInputs);

    if (quirks & GameQuirk::ForceBorderlessWhenUsingXeFG && !Config::Instance()->FGXeFGForceBorderless.has_value() &&
        State::Instance().activeFgOutput == FGOutput::XeFG && State::Instance().activeFgInput != FGInput::NoFG &&
        State::Instance().activeFgInput != FGInput::NvngxFG)
    {
        Config::Instance()->FGXeFGForceBorderless.set_volatile_value(true);
    }
    else
        quirks.reset(GameQuirk::ForceBorderlessWhenUsingXeFG);

    if (quirks & GameQuirk::OverrideVsyncWhenUsingXeFG && !Config::Instance()->OverrideVsync.has_value() &&
        State::Instance().activeFgOutput == FGOutput::XeFG && State::Instance().activeFgInput != FGInput::NoFG &&
        State::Instance().activeFgInput != FGInput::NvngxFG)
    {
        Config::Instance()->OverrideVsync.set_volatile_value(true);
    }
    else
        quirks.reset(GameQuirk::OverrideVsyncWhenUsingXeFG);

    if (quirks & GameQuirk::DisableResizeSkip && !Config::Instance()->FGSkipResizeBuffers.has_value())
    {
        Config::Instance()->FGSkipResizeBuffers.set_volatile_value(false);
    }
    else
        quirks.reset(GameQuirk::DisableResizeSkip);

    if (quirks & GameQuirk::SpoofRegistry && !Config::Instance()->SpoofRegistry.has_value())
    {
        Config::Instance()->SpoofRegistry.set_volatile_value(true);
    }
    else
        quirks.reset(GameQuirk::SpoofRegistry);

    if (quirks & GameQuirk::DisableFakenvapi && !Config::Instance()->UseFakenvapi.has_value())
    {
        Config::Instance()->UseFakenvapi.set_volatile_value(false);
    }
    else
        quirks.reset(GameQuirk::DisableFakenvapi);

    if (quirks & GameQuirk::DoNotPreserveFGSwapChain && !Config::Instance()->FGPreserveSwapChain.has_value())
    {
        Config::Instance()->FGPreserveSwapChain.set_volatile_value(false);
    }
    else
        quirks.reset(GameQuirk::DoNotPreserveFGSwapChain);

    // misc/REFrameworkCompat.h, decided before the menu quirk below that it changes
    REFrameworkCompat::Init(quirks[GameQuirk::REFrameworkCompat]);

    if (REFrameworkCompat::Active())
    {
        // The old overlay menu switches frame generation off, and that REFramework is there to draw on XeFG's frames
        if (quirks & GameQuirk::OldOverlayMenu && !Config::Instance()->OverlayMenu.has_value())
        {
            quirks.reset(GameQuirk::OldOverlayMenu);
            LOG_INFO("REFramework compatibility: old overlay menu quirk not applied (it turns frame generation off)");
        }

        // REFramework's menu is on Insert as well: with both on one key it misses every other press in some games
        if (!Config::Instance()->ShortcutKey.has_value())
        {
            Config::Instance()->ShortcutKey.set_volatile_value(VK_HOME);
            LOG_INFO("REFramework compatibility: menu key is Home (REFramework uses Insert; ShortcutKey changes it)");
        }
    }
    else
    {
        quirks.reset(GameQuirk::REFrameworkCompat);
    }

    if (quirks & GameQuirk::OldOverlayMenu && !Config::Instance()->OverlayMenu.has_value())
    {
        Config::Instance()->OverlayMenu.set_volatile_value(false);
    }
    else
        quirks.reset(GameQuirk::OldOverlayMenu);

    if (quirks & GameQuirk::DoNotLoadAmdxc64 && !Config::Instance()->Fsr4DoNotLoadAmdxc64.has_value())
    {
        Config::Instance()->Fsr4DoNotLoadAmdxc64.set_volatile_value(true);
    }
    else
        quirks.reset(GameQuirk::DoNotLoadAmdxc64);

    // For Luma, we assume if Luma addon in game folder it's used
    const auto dir = Util::ExePath().parent_path();
    bool lumaDetected = false;

    for (const auto& entry : std::filesystem::directory_iterator(dir))
    {
        if (!entry.is_regular_file())
            continue;

        const auto& path = entry.path();
        if (path.extension() == L".addon" || path.extension() == L".addon64")
        {
            const auto fname = Util::ToLower(path.filename().wstring());

            // starts with "luma-" or "dlaa-inject"
            if ((fname.rfind(L"luma-", 0) == 0) || (fname.rfind(L"dlaa-inject", 0) == 0))
            {
                lumaDetected = true;
                break;
            }
        }
    }

    if (lumaDetected)
    {
        if (!Config::Instance()->DxgiSpoofing.has_value())
        {
            LOG_INFO("Luma detected, disabling DxgiSpoofing");
            State::Instance().detectedQuirks.push_back("Luma detected, disabling DxgiSpoofing");
            Config::Instance()->DxgiSpoofing.set_volatile_value(false);
        }

        if (!Config::Instance()->DontUseNTShared.has_value())
        {
            LOG_INFO("Luma detected, enabling DontUseNTShared");
            State::Instance().detectedQuirks.push_back("Luma detected, enabling DontUseNTShared");
            Config::Instance()->DontUseNTShared.set_volatile_value(true);
        }

        if (!Config::Instance()->CreateD3D12DeviceForLuma.has_value())
        {
            quirks |= GameQuirk::LoadD3D12Manually;

            if (Config::Instance()->LoadReShade.value_or_default())
            {
                Config::Instance()->CreateD3D12DeviceForLuma.set_volatile_value(true);
                quirks |= GameQuirk::CreateD3D12DeviceForLuma;
            }
        }
    }

    // For Sekiro TSR
    if (std::filesystem::exists(Util::ExePath().parent_path() / L"SekiroTSRLoader.addon"))
    {
        if (!Config::Instance()->DxgiSpoofing.has_value())
        {
            LOG_INFO("Sekiro TSR detected, disabling DxgiSpoofing");
            State::Instance().detectedQuirks.push_back("Luma UE detected, disabling DxgiSpoofing");
            Config::Instance()->DxgiSpoofing.set_volatile_value(false);
        }

        if (!Config::Instance()->DontUseNTShared.has_value())
        {
            LOG_INFO("Sekiro TSR detected, enabling DontUseNTShared");
            State::Instance().detectedQuirks.push_back("Sekiro TSR detected, enabling DontUseNTShared");
            Config::Instance()->DontUseNTShared.set_volatile_value(true);
        }

        if (!Config::Instance()->CreateD3D12DeviceForLuma.has_value())
        {
            quirks |= GameQuirk::LoadD3D12Manually;

            if (Config::Instance()->LoadReShade.value_or_default())
            {
                Config::Instance()->CreateD3D12DeviceForLuma.set_volatile_value(true);
                quirks |= GameQuirk::CreateD3D12DeviceForLuma;
            }
        }
    }

    // if (!Config::Instance()->DxgiFactoryWrapping.has_value() && Config::Instance()->LoadReShade.value_or_default() &&
    //     quirks & GameQuirk::CreateD3D12DeviceForLuma && State::Instance().activeFgInput != FGInput::NoFG &&
    //     State::Instance().activeFgInput != FGInput::NvngxFG)
    //{
    //     Config::Instance()->DxgiFactoryWrapping.set_volatile_value(true);
    //     State::Instance().detectedQuirks.push_back("Factory wrapping enabled due to delayed ReShade + FG");
    //     LOG_INFO("Factory wrapping enabled due to delayed ReShade + FG");
    // }

    if (Config::Instance()->LoadSpecialK.value_or_default() && State::Instance().activeFgInput != FGInput::NoFG &&
        State::Instance().activeFgInput != FGInput::NvngxFG)
    {
        Config::Instance()->LoadSpecialK.set_volatile_value(false);
        State::Instance().detectedQuirks.push_back("FG Inputs are enabled, LoadSpecialK disabled");
        LOG_INFO("FG Inputs are enabled, LoadSpecialK disabled");
    }

    State::Instance().gameQuirks = quirks;

    printQuirks(quirks);
}

void CheckForExcludedProcess()
{
    std::wstring exeLower = Util::ExePath().filename().wstring();
    std::transform(exeLower.begin(), exeLower.end(), exeLower.begin(), ::towlower);

    // If target process is set, only that process is hooked
    if (Config::Instance()->TargetProcess.has_value())
    {
        // Config reads string as lowercase already
        std::wstring targetProcess = Config::Instance()->TargetProcess.value();

        if (exeLower != targetProcess)
        {
            _passThruMode = true;
            return;
        }
    }

    // Config reads string as lowercase already
    static const std::wstring exclusionList = Config::Instance()->ProcessExclusionList.value_or_default() + L"|";

    static std::vector<std::wstring> exclusions = []()
    {
        std::vector<std::wstring> result;
        size_t start = 0, end;

        while ((end = exclusionList.find(L'|', start)) != std::wstring::npos)
        {
            result.emplace_back(exclusionList.substr(start, end - start));
            start = end + 1;
        }
        return result;
    }();

    for (auto& e : exclusions)
    {
        if (exeLower == e)
        {
            _passThruMode = true;
            return;
        }
    }

    _passThruMode = false;
}

void CheckMemoryForProxies()
{
    FfxApiProxy::InitFfxDx12();
    FfxApiProxy::InitFfxDx12_SR();
    FfxApiProxy::InitFfxDx12_FG();
    FfxApiProxy::InitFfxDx12_Denoiser();
    FfxApiProxy::InitFfxDx12_Radiance();

    XeSSProxy::InitXeSS();
    XeSSProxy::InitXeSSDx11();
    XeFGProxy::InitXeFG();
    XeLLProxy::InitXeLL();

    XellHooks::Hook();

    NVNGXProxy::InitNVNGX();
}

// Optional Genshin native provenance must be prepared before the game's first
// D3D11 factory. This existing worker starts after DLL_PROCESS_ATTACH returns;
// no signature I/O, waiting or GPU enumeration is added under loader lock.
// R764: bounded post-refusal observation. No result is consumed by admission.
static void ObserveGenshinOwnedPatchFailure()
{
    namespace d = GenshinNativeQuery::detail;
    struct Row {
        unsigned index=0;const char* kind="none";const char* symbol="none";
        uintptr_t entry=0,slot=0,expected=0,actual=0;unsigned mismatch=UINT_MAX;
        uintptr_t firstTarget=0,finalTarget=0,ownRelay=0;unsigned followed=0;DWORD finalType=0;
        std::array<char,128> finalOwner{};
        std::array<char,128> owner{};bool read=false;
    };
    std::array<Row,16> rows{};unsigned count=0,total=0,patches=0,initialized=0;
    uint64_t generation=0;bool captured=false;
    {
        d::Guard guard;
        if (guard.held) {
            captured=true;generation=d::store.generation.load();initialized=d::store.initialized.load();
            const auto& ledger=d::Ledger();patches=ledger.patchCount;
            if (patches<=ledger.patches.size()) for (unsigned i=0;i<patches;++i) {
                const auto& p=ledger.patches[i];Row row{};row.index=i;
                row.entry=reinterpret_cast<uintptr_t>(p.entry);row.symbol=p.exportName?p.exportName:"none";
                const auto pointer=[&](const char* kind,const void* slot,const void* wanted) {
                    uintptr_t actual=0;const bool read=d::Memory(slot,&actual,sizeof(actual));
                    if (read && actual==reinterpret_cast<uintptr_t>(wanted)) return false;
                    row.kind=kind;row.slot=reinterpret_cast<uintptr_t>(slot);row.expected=reinterpret_cast<uintptr_t>(wanted);row.actual=actual;row.read=read;return true;
                };
                const auto bytes=[&](const char* kind,const void* at,const unsigned char* wanted,size_t n) {
                    std::array<unsigned char,96> actual{};
                    const bool read=n<=actual.size() && d::Memory(at,actual.data(),n);
                    if (read && !std::memcmp(actual.data(),wanted,n)) return false;
                    row.kind=kind;row.slot=reinterpret_cast<uintptr_t>(at);row.read=read;
                    if (read) for (unsigned j=0;j<n;++j) if (actual[j]!=wanted[j]) {
                        row.mismatch=j;row.expected=wanted[j];row.actual=actual[j];break;
                    }
                    return true;
                };
                bool failed=pointer("original-slot",p.original,p.trampoline);
                if (!failed && p.executed) failed=pointer("executed-slot",p.executed,p.trampoline);
                if (!failed && p.mirror) failed=pointer("mirror-slot",p.mirror,p.trampoline);
                if (!failed && p.hopCount>p.hops.size()) {failed=true;row.kind="invalid-hop-count";}
                for (unsigned j=0;!failed && j<p.hopCount;++j) {
                    const auto& hop=p.hops[j];failed=bytes("hop-code",hop.entry,hop.code.data(),hop.size);
                    if (!failed && hop.iat) failed=pointer("hop-slot",hop.iat,hop.target);
                }
                if (!failed) failed=bytes("entry-edit",p.entry,p.edit.data(),p.size);
                if (!failed) failed=bytes("trampoline-code",p.trampoline,p.code.data(),p.code.size());
                if (!failed) failed=bytes("own-hook-code",p.hook,p.ownEntry.data(),p.ownEntry.size());
                if (failed) {
                    ++total;
                    row.ownRelay=reinterpret_cast<uintptr_t>(p.trampoline)+88;
                    // Two bounded noncalling reads of E9/FF25 branch encodings.
                    uintptr_t cursor=row.entry;
                    for (unsigned step=0;step<2;++step) {
                        std::array<unsigned char,6> code{};uintptr_t next=0;
                        if (!d::Memory(reinterpret_cast<const void*>(cursor),code.data(),code.size())) break;
                        if (code[0]==0xe9) next=cursor+5+static_cast<intptr_t>(d::Read<int32_t>(code.data()+1));
                        else if (code[0]==0xff && code[1]==0x25) {
                            const auto cell=cursor+6+static_cast<intptr_t>(d::Read<int32_t>(code.data()+2));
                            if (!d::Memory(reinterpret_cast<const void*>(cell),&next,sizeof(next))) break;
                        } else break;
                        if (!next || next==cursor) break;
                        if (!step) row.firstTarget=next;
                        cursor=next;row.finalTarget=next;++row.followed;
                    }
                    MEMORY_BASIC_INFORMATION targetMemory{};
                    if (row.finalTarget && VirtualQuery(reinterpret_cast<const void*>(row.finalTarget),&targetMemory,sizeof(targetMemory))==sizeof(targetMemory)) {
                        row.finalType=targetMemory.Type;
                        if (targetMemory.Type==MEM_IMAGE) d::ImportModuleName(static_cast<HMODULE>(targetMemory.AllocationBase),row.finalOwner);
                    }
                    MEMORY_BASIC_INFORMATION memory{};
                    if (VirtualQuery(p.entry,&memory,sizeof(memory))==sizeof(memory) && memory.Type==MEM_IMAGE)
                        d::ImportModuleName(static_cast<HMODULE>(memory.AllocationBase),row.owner);
                    if (count<rows.size()) rows[count++]=row;
                }
            }
        }
    }
    LOG_WARN("Genshin owned patch observation: pid={} captured={} generation={} initialized={} patches={} failures={} recorded={} post_refusal_only=true",
             GetCurrentProcessId(),captured,generation,initialized,patches,total,count);
    for (unsigned i=0;i<count;++i) {const auto& row=rows[i];
        LOG_WARN("Genshin owned patch mismatch: pid={} index={} kind={} symbol={} owner={} entry={:X} slot={:X} expected={:X} actual={:X} mismatch_offset={} readable={} observation_only=true",
                 GetCurrentProcessId(),row.index,row.kind,row.symbol,row.owner.data(),row.entry,row.slot,row.expected,row.actual,row.mismatch,row.read);
        LOG_WARN("Genshin owned patch target: pid={} index={} own_relay={:X} first_target={:X} final_target={:X} followed={} final_type={:X} final_owner={} observation_only=true no_callable_authority=true",
                 GetCurrentProcessId(),row.index,row.ownRelay,row.firstTarget,row.finalTarget,row.followed,row.finalType,row.finalOwner.data());
    }
}

// R773: bounded read-only post-refusal data; never consumed by authentication.
static void ObserveGenshinInstallerBytes()
{
    namespace d=GenshinNativeQuery::detail;
    namespace f=d::file_binding;
    const auto app=GetModuleHandleW(L"apphelp.dll");
    auto image=std::make_unique<d::Image>();
    const bool opened=app && image->Open(app);
    LOG_WARN("Genshin installer observation: pid={} opened={} profile={} observation_only=true no_callable_authority=true",
             GetCurrentProcessId(),opened,opened && d::shim_binding::ProfileImage(*image,d::shim_observation::apphelpProfile));
    if(!opened)return;
    const auto base=reinterpret_cast<uintptr_t>(app);
    for(unsigned rowIndex=0;rowIndex<f::Installers().size();++rowIndex){const auto& spec=f::Installers()[rowIndex];
        std::array<unsigned char,64> live{};
        const size_t size=spec.end-spec.wrapper;
        const bool read=size<=live.size() && image->Range(spec.wrapper,size) && d::Memory(reinterpret_cast<const void*>(base+spec.wrapper),live.data(),size);
        unsigned mismatch=UINT_MAX;unsigned expected=0,actual=0;
        if(read)for(unsigned i=0;i<size;++i)if(live[i]!=image->clean[spec.wrapper+i]){mismatch=i;expected=image->clean[spec.wrapper+i];actual=live[i];break;}
        unsigned ownCount=0;bool ownIntact=true;std::array<unsigned,16> ownIndices{};unsigned recorded=0;
        {
            d::Guard guard;
            if(guard.held){const auto& ledger=d::Ledger();
                if(ledger.patchCount<=ledger.patches.size())for(unsigned i=0;i<ledger.patchCount;++i){const auto& patch=ledger.patches[i];
                    const auto begin=reinterpret_cast<uintptr_t>(patch.entry);
                    if(begin<base+spec.end && begin+patch.size>base+spec.wrapper){
                        ++ownCount;ownIntact=ownIntact && d::PatchIntact(patch);if(recorded<ownIndices.size())ownIndices[recorded++]=i;
                    }
                }
            }else ownIntact=false;
        }
        uintptr_t cursor=base+spec.wrapper,first=0,last=0;unsigned hops=0;DWORD targetType=0;std::array<char,128> targetOwner{};
        for(unsigned step=0;step<2;++step){std::array<unsigned char,16> code{};uintptr_t next=0;
            if(!d::Memory(reinterpret_cast<const void*>(cursor),code.data(),code.size()))break;
            if(code[0]==0xe9)next=cursor+5+static_cast<intptr_t>(d::Read<int32_t>(code.data()+1));
            else if(code[0]==0xff && code[1]==0x25){const auto cell=cursor+6+static_cast<intptr_t>(d::Read<int32_t>(code.data()+2));
                if(!d::Memory(reinterpret_cast<const void*>(cell),&next,sizeof(next)))break;}
            else if(code[0]==0x48 && code[1]==0xb8 && code[10]==0xff && code[11]==0xe0)next=d::Read<uintptr_t>(code.data()+2);
            else break;
            if(!next || next==cursor)break;if(!step)first=next;last=cursor=next;++hops;
        }
        MEMORY_BASIC_INFORMATION memory{};
        if(last && VirtualQuery(reinterpret_cast<const void*>(last),&memory,sizeof(memory))==sizeof(memory)){
            targetType=memory.Type;if(memory.Type==MEM_IMAGE)d::ImportModuleName(static_cast<HMODULE>(memory.AllocationBase),targetOwner);
        }
        LOG_WARN("Genshin installer wrapper: pid={} symbol={} rva={:X} bytes={} readable={} mismatch_offset={} expected={:X} actual={:X} live_prefix={:X} own_overlap={} own_intact={} first_target={:X} final_target={:X} followed={} final_type={:X} final_owner={} observation_only=true",
                 GetCurrentProcessId(),spec.symbol,spec.wrapper,size,read,mismatch,expected,actual,read?d::Read<uint64_t>(live.data()):0,
                 ownCount,ownIntact,first,last,hops,targetType,targetOwner.data());
        for(unsigned i=0;i<recorded;++i)LOG_WARN("Genshin installer own overlap: pid={} symbol={} patch_index={} observation_only=true",GetCurrentProcessId(),spec.symbol,ownIndices[i]);
        uintptr_t table=0;std::array<uintptr_t,6> row{};
        const bool rowRead=d::Memory(reinterpret_cast<const void*>(base+0x79420),&table,sizeof(table)) && table && table<=UINTPTR_MAX-spec.row &&
            d::Memory(reinterpret_cast<const void*>(table+spec.row),row.data(),sizeof(row));
        std::array<char,128> originalOwner{};MEMORY_BASIC_INFORMATION originalMemory{};
        if(rowRead && row[3] && VirtualQuery(reinterpret_cast<const void*>(row[3]),&originalMemory,sizeof(originalMemory))==sizeof(originalMemory) && originalMemory.Type==MEM_IMAGE)
            d::ImportModuleName(static_cast<HMODULE>(originalMemory.AllocationBase),originalOwner);
        LOG_WARN("Genshin installer row: pid={} symbol={} readable={} table={:X} dll={:X} name={:X} wrapper={:X} original={:X} original_owner={} observation_only=true no_callable_authority=true",
                 GetCurrentProcessId(),spec.symbol,rowRead,table,row[0],row[1],row[2],row[3],originalOwner.data());
    }
}

// R041: consume the immutable ORIGINAL failure after asynchronous publication.
// Later Open/code comparisons cannot recover the original predicate or trust result.
static void ObserveGenshinFailedImage() noexcept
{
    const auto* first=GenshinNativeQuery::FirstRevocation();
    if(!first)return;
    static std::atomic_flag logged=ATOMIC_FLAG_INIT;
    if(logged.test_and_set())return;
    try {
        // Hold the logger for this synchronous call even if its registry entry changes.
        const auto logger=spdlog::default_logger();if(!logger)return;
        const auto module=first->module;
        std::array<wchar_t,32768> wide{};std::array<char,32768> path{};
        const auto length=module?GetModuleFileNameW(module,wide.data(),static_cast<DWORD>(wide.size())):0;
        if(length && length<wide.size())WideCharToMultiByte(CP_UTF8,0,wide.data(),length,path.data(),static_cast<int>(path.size()-1),nullptr,nullptr);
        const auto& o=first->image;const auto& trust=first->trust;
        logger->warn("ObserveGenshinFailedImage Genshin failed image observation: pid={} thread={} module={:X} path={} reason={} original_open_captured={} predicate={} offset={:X} actual={:X} expected={:X} win32={} "
                 "embedded_attempted={} embedded_status={:X} embedded_win32={} embedded_microsoft={} catalog_attempts={} catalog_attempted={} catalog_status={:X} catalog_win32={} catalog_microsoft={} catalog_stage={} catalog_stage_win32={} "
                 "repeated_open=false post_refusal_only=true original_predicate_unknown={} no_callable_authority=true",
                 GetCurrentProcessId(),first->thread,reinterpret_cast<uintptr_t>(module),path.data(),first->reason,first->hasImage,
                 first->hasImage?o.predicate:"not-an-image-open-refusal",o.offset,o.actual,o.expected,o.win32,
                 trust.embedded.attempted,static_cast<unsigned>(trust.embedded.status),trust.embedded.win32,trust.embedded.microsoft,
                 trust.catalogAttempts,trust.catalog.attempted,static_cast<unsigned>(trust.catalog.status),trust.catalog.win32,trust.catalog.microsoft,
                 trust.catalogStage,trust.catalogWin32,!first->hasImage);
        logger->warn("ObserveGenshinFailedImage Genshin native first_revocation: pid={} thread={} file={} line={} function={} observation_only=true",
                 GetCurrentProcessId(),first->thread,first->file,first->line,first->function);
        const auto& c=first->origin;
        logger->warn("ObserveGenshinFailedImage Genshin native collection origin: pid={} thread={} patch={} role={} export={} export_truncated={} own_setup={} "
                     "producer_file={} producer_line={} producer_function={} collection_file={} collection_line={} collection_function={} "
                     "request_file={} request_line={} request_function={} depth={} parent={:X} module={:X} hop={} "
                     "address={} requested={} normalized_entry={} hook={} trampoline={} original_slot={} executed_slot={} mirror_slot={} declared_module={:X} "
                     "origin_captured_at_refusal=true no_callable_authority=true",
                     GetCurrentProcessId(),first->thread,c.patch,c.role,c.exportName.data(),c.exportNameTruncated,c.ownSetup,
                     c.enrollment.file_name(),c.enrollment.line(),c.enrollment.function_name(),
                     c.collection.file_name(),c.collection.line(),c.collection.function_name(),
                     c.request.file_name(),c.request.line(),c.request.function_name(),c.depth,
                     reinterpret_cast<uintptr_t>(c.parent),reinterpret_cast<uintptr_t>(c.current),c.hopIndex,
                     c.address,c.requested,c.entry,c.hook,c.trampoline,c.original,c.executed,c.mirror,
                     reinterpret_cast<uintptr_t>(c.declaredModule));
    }catch(...){/* Optional diagnostics never alter the original refusal. */}
}

// R728: one immutable observation of the first failed Setup import.
// Shared by the synchronous tail and the existing asynchronous refusal observer.
// No address in this receipt is an alternate binding or admission path.
static void ObserveGenshinImportFailure() noexcept
{
    static std::atomic_flag importFailureLogged = ATOMIC_FLAG_INIT;
    const auto* failure = GenshinNativeQuery::FirstImportFailure();
    // Do not consume the once flag before the immutable receipt is published.
    if (!failure || importFailureLogged.test_and_set(std::memory_order_relaxed))
        return;
    try
    {
        LOG_WARN("Genshin native import first_failure: pid={} thread={} stage={} prior_reason={} "
                 "dll={} symbol={} ordinal={} importer={:X} importer_name={} nominal={:X} slot={:X} "
                 "expected_known={} expected={:X} expected_owner={:X} expected_owner_name={} expected_rva={:X} kind={} "
                 "actual_known={} actual={:X} actual_owner_known={} actual_owner={:X} actual_owner_name={} actual_rva={:X} "
                 "queried_actual_owner={:X} resolver_hop={} resolver_module={:X} resolver_dll={} resolver_symbol={} resolver_ordinal={} "
                 "export_index={} forward={} resolver_actual_known={} resolver_actual={:X} resolver_owner_known={} "
                 "resolver_owner={:X} resolver_owner_name={} resolver_rva={:X} text_truncated={} "
                 "observed_owner_source=VirtualQuery-MEM_IMAGE observation_only=true",
                 failure->pid, failure->thread, failure->stage, failure->priorReason,
                 failure->dll.data(), failure->symbol.data(), failure->ordinal,
                 reinterpret_cast<size_t>(failure->importer), failure->importerName.data(),
                 reinterpret_cast<size_t>(failure->nominal), reinterpret_cast<size_t>(failure->slot),
                 failure->expectedKnown, failure->expected, reinterpret_cast<size_t>(failure->expectedOwner),
                 failure->expectedOwnerName.data(), failure->expectedRva, static_cast<unsigned>(failure->expectedKind),
                 failure->actualKnown, failure->actual.value, failure->actual.ownerKnown, failure->actual.owner,
                 failure->actual.ownerName.data(), failure->actual.rva, reinterpret_cast<size_t>(failure->queriedActualOwner),
                 failure->hop, reinterpret_cast<size_t>(failure->resolverModule), failure->resolverDll.data(),
                 failure->resolverSymbol.data(), failure->resolverOrdinal, failure->exportIndex, failure->forward.data(),
                 failure->resolverActualKnown, failure->resolverActual.value, failure->resolverActual.ownerKnown,
                 failure->resolverActual.owner, failure->resolverActual.ownerName.data(), failure->resolverActual.rva,
                 failure->textTruncated);

        // R740: separate immutable observations; preserve the R728 stage token.
        const auto& c = failure->corroboration;
        LOG_WARN("Genshin native import corroboration_snapshot: pid={} thread={} stage={} "
                 "eat_slot={} eat_address={:X} eat_read={} clean_eat_rva={:X} live_eat_rva={:X} "
                 "iat_sample={} iat_slot={} iat_read={} iat_now={:X} observation_only=true",
                 failure->pid, failure->thread, failure->stage, c.eatSlot, c.eatSlotAddress,
                 c.eatSlotRead, c.cleanEatRva, c.liveEatRva, c.iatSample, c.iatSlot, c.iatSlotRead, c.iatNow);
        const auto& s = failure->shim;
        LOG_WARN("Genshin native shim snapshot: pid={} thread={} profile={} status={} "
                 "wrapper_disk_read={} wrapper_disk_match={} wrapper_read={} wrapper_match={} wrapper_code64={} "
                 "locators_read={} locators_match={} thunks_read={} thunks_match={} "
                 "xfg_uses_declared_cf={} whole_image_identity=UNKNOWN observation_only=true",
                 failure->pid, failure->thread, s.profile, s.status, s.wrapperDiskRead, s.wrapperDiskMatch,
                 s.wrapperRead, s.wrapperMatch, s.wrapperBytes.data(), s.locatorsRead, s.locatorsMatch,
                 s.thunksRead, s.thunksMatch, s.xfgUsesDeclaredCf);
        const auto module = [&](const char* role, const auto& m) {
            LOG_WARN("Genshin native shim module: pid={} role={} base={:X} status={} file_size={} "
                     "timestamp={:X} image_size={:X} fixed_version_ms={:X} fixed_version_ls={:X} "
                     "disk_read={} disk_match={} live_read={} live_match={} error={} "
                     "identity_scope=selected_headers_version_code observation_only=true",
                     failure->pid, role, m.base, m.status, m.fileSize, m.timestamp, m.imageSize,
                     m.versionMS, m.versionLS, m.diskRead, m.diskMatch, m.liveRead, m.liveMatch, m.error);
        };
        module("apphelp", s.apphelp); module("ntdll", s.ntdll);
        const auto slot = [&](const char* role, const auto& v) {
            LOG_WARN("Genshin native shim slot: pid={} role={} address={:X} value={:X} read={} status={} "
                     "error={} observation_only=true", failure->pid, role, v.address, v.value, v.read, v.status, v.error);
        };
        slot("table_root", s.tableRoot); slot("xfg_dispatch", s.xfgSlot); slot("declared_cf_dispatch", s.cfSlot);
        LOG_WARN("Genshin native shim row: pid={} address={:X} region_read={} allocation={:X} type={:X} "
                 "state={:X} protect={:X} read={} selectors_match={} error={} "
                 "q0={:X} q1={:X} q2={:X} original={:X} q4={:X} q5={:X} observation_only=true",
                 failure->pid, s.rowAddress, s.tableRegionRead, s.tableAllocation, s.tableType, s.tableState,
                 s.tableProtect, s.rowRead, s.rowSelectorsMatch, s.rowError,
                 s.row[0], s.row[1], s.row[2], s.row[3], s.row[4], s.row[5]);
        const auto code = [&](const char* role, const auto& v) {
            LOG_WARN("Genshin native shim code: pid={} role={} target={:X} owner_known={} owner={:X} owner_name={} "
                     "rva={:X} type={:X} protect={:X} read={} disk_read={} comparison_bytes={} comparison_source={} "
                     "selected_code_match={} status={} "
                     "code64={} error={} observation_only=true",
                     failure->pid, role, v.address.value, v.address.ownerKnown, v.address.owner, v.address.ownerName.data(),
                     v.address.rva, v.address.memoryType, v.protect, v.read, v.diskRead, v.comparisonBytes,
                     v.comparisonSource, v.comparisonMatch, v.status, v.bytes.data(), v.error);
        };
        code("table_original", s.original); code("xfg_dispatch", s.xfg); code("declared_cf_dispatch", s.cf);
    }
    catch (...) { /* Optional diagnostics never alter the original refusal. */ }
}

static void PrepareGenshinNativeQuery()
{
    if constexpr (!F6InputPolicy::FsrBridgeEnabled) return;
    auto* cfg = Config::Instance();
    if (!GenshinFsr::IsGenshinExecutable(Util::ExePath().wstring()) ||
        !cfg->AcquireGenshinControls().Active() || !cfg->OverlayMenu.value_or_default())
        return;

    // Keep optional authentication on this worker. Existing startup/library
    // paths retain sole responsibility for factory hooking: do not introduce a
    // parallel Hook call against their non-atomic installer slots. Own::Batch
    // authenticates later own edits after Setup as well as edits already made.
    // No previously exposed device/context can acquire a certificate later.
    const auto observeNativeFailure=+[]()noexcept {
        ObserveGenshinFailedImage();
        ObserveGenshinImportFailure();
        const auto* first=GenshinNativeQuery::FirstRevocation();
        if(!first || !first->setupPatchFailure.captured)return;
        static std::atomic_flag reported=ATOMIC_FLAG_INIT;
        if(reported.test_and_set(std::memory_order_relaxed))return;
        const auto& o=first->setupPatchFailure;const auto& origin=first->origin;
        try {
            LOG_WARN("R041 setup preimage first refusal: pid={} thread={} predicate={} attempt={} index={} owner={} rva={:X} size={} mismatch_offset={} saved_byte={:X} clean_byte={:X} install_captured={} install_byte={:X} restore_matches_install={} install_generation={} snapshot_generation={} failure_generation={} publication_generation={} saved_prefix_le={:X} clean_prefix_le={:X} entry={:X} requested={:X} producer_file={} producer_line={} observation_only=true no_callable_authority=true",
                GetCurrentProcessId(),first->thread,o.predicate,o.attempt,o.index,o.owner,o.rva,o.size,o.mismatchOffset,
                o.savedByte,o.cleanByte,o.installPreimageCaptured,o.installByte,o.restoreMatchesInstall,o.installGeneration,
                o.snapshotGeneration,o.failureGeneration,o.publicationGeneration,o.savedPrefix,o.cleanPrefix,
                reinterpret_cast<uintptr_t>(origin.entry),reinterpret_cast<uintptr_t>(origin.requested),
                origin.enrollment.file_name(),origin.enrollment.line());
        }catch(...){}
    };
    GenshinNativeQuery::SetFirstRevocationObserver(observeNativeFailure);
    GenshinNativeQuery::StartupInstallScope nativeQueryStartup;
    const auto runtime = NtdllProxy::LoadLibraryExW_Ldr(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!runtime)
    {
        LOG_WARN("Genshin early native setup: system d3d11 load failed; optional depth/FG remains gated");
        return;
    }
    // Retain the loader reference for process lifetime, matching existing
    // graphics runtime ownership. This does not create a device or run a game.
    GenshinNativeQuery::SetupObservation setupObservation;
    const bool ready = GenshinNativeQuery::Setup(runtime,&setupObservation);
    LOG_INFO("R041 worker native preparation: pid={} ready={} predicate={} attempts={} generation_conflicts={} snapshot_generation={} return_generation={} state_sample={} setup_running_sample={} busy_sample={} deferred_sample={} install_scopes_sample={} observation_only=true no_birth_authority=true",
        GetCurrentProcessId(),ready,setupObservation.predicate,setupObservation.attempts,setupObservation.generationConflicts,
        setupObservation.snapshotGeneration,setupObservation.returnGeneration,setupObservation.state,setupObservation.running,
        setupObservation.busy,setupObservation.deferred,setupObservation.installScopes);
    // Covers a refusal already published before this synchronous Setup entry.
    observeNativeFailure();
    LOG_INFO("Genshin early native setup: pid={} ready={} reason={} runtime={:X}",
             GetCurrentProcessId(), ready, GenshinNativeQuery::Failure(), reinterpret_cast<size_t>(runtime));
    if (!ready)
    {
        ObserveGenshinOwnedPatchFailure();
        ObserveGenshinInstallerBytes();
    }
    if (!ready)
        ObserveGenshinImportFailure();
}

DWORD WINAPI getGpuInfo(LPVOID hModuleVoid)
{
    startupav::SetPhase(startupav::Phase::WorkerEntry);
    startupav::Prepare(dllModule);
    const bool hoYoEnabled = Config::Instance()->HoYoShadeEnabled.value_or_default();
    const unsigned hoYoStage = static_cast<unsigned>(Config::Instance()->HoYoShadeStage.value_or_default());
    // This existing post-attach worker is outside the loader callout. Prepare
    // provenance before optional core startup can consume a creation factory.
    // Readiness is still required by NativeCall; moving order grants no birth.
    PrepareGenshinNativeQuery();
    HoYoShade::Initialize(dllModule, hoYoEnabled, hoYoStage);
    LogHoYoShadeStartupConfig(hoYoEnabled, hoYoStage);
    auto primaryGpu = IdentifyGpu::getPrimaryGpu();

    // We don't yet know if the GPU supports FSR 4 so hook any AMD
    if (primaryGpu.vendorId == VendorId::AMD)
        Amdxc64Hooks::Init();

    else if (Config::Instance()->Fsr4ForceModel.value_or_default() == FSR4Support::INT8)
    {
        // We need spoofing hooks for FFX but want to avoid spoofing for the rest of the game
        if (!Config::Instance()->DxgiSpoofing.value_or_default())
        {
            std::wstring wname = string_to_wstring(primaryGpu.name);
            Config::Instance()->SpoofedVendorId.set_volatile_value(primaryGpu.vendorId);
            Config::Instance()->SpoofedDeviceId.set_volatile_value(primaryGpu.deviceId);
            Config::Instance()->SpoofedGPUName.set_volatile_value(wname);
        }

        Config::Instance()->DxgiSpoofing.set_volatile_value(true);
    }

    // If DX12 already loaded then grab the full GPU info right away
    if (hModuleVoid)
        IdentifyGpu::updateD3d12Capabilities();

    // This existing worker runs after DLL_PROCESS_ATTACH has returned. GPU
    // enumeration and loading another graphics proxy must not run in DllMain.

    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID /*lpReserved*/)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
    {
        // Publish no hooks/worker callbacks unless their code lifetime is fixed.
        if (!ModuleLifetime::PinCallbacks(hModule)) return FALSE;
        DisableThreadLibraryCalls(hModule);

        HMODULE handle = nullptr;
        OSVERSIONINFOW winVer { 0 };

        dllModule = hModule;
        exeModule = GetModuleHandle(nullptr);
        processId = GetCurrentProcessId();

        // Main Opti DLL path
        if (!Config::Instance()->MainDllPath.has_value())
        {
            Config::Instance()->MainDllPath.set_volatile_value(L"OptiScaler");
        }

        if (std::filesystem::path mainDllPath(Config::Instance()->MainDllPath.value()); mainDllPath.is_relative())
        {
            Config::Instance()->MainDllPath.set_volatile_value(Util::ExePath().parent_path() / mainDllPath);
        }

        // If path is invalid or doesn't exist, use the exe folder as main
        if (!std::filesystem::exists(Config::Instance()->MainDllPath.value()) ||
            !std::filesystem::is_directory(Config::Instance()->MainDllPath.value()))
        {
            Config::Instance()->MainDllPath.set_volatile_value(Util::ExePath().parent_path());
        }

        // Clean up path
        Config::Instance()->MainDllPath.set_volatile_value(
            std::filesystem::absolute(Config::Instance()->MainDllPath.value()));

        // If path is not set or incorrect
        if (!Config::Instance()->PluginPath.has_value() ||
            (!std::filesystem::exists(Config::Instance()->PluginPath.value()) ||
             !std::filesystem::is_directory(Config::Instance()->PluginPath.value())))
        {
            Config::Instance()->PluginPath.set_volatile_value(
                std::filesystem::path(Config::Instance()->MainDllPath.value()) / L"plugins");
        }

        CheckForExcludedProcess();

        if (_passThruMode)
        {
            NtdllProxy::Init();
            KernelBaseProxy::Init();
            Kernel32Proxy::Init();

            CheckWorkingMode();
            return true;
        }

#ifdef _DEBUG // VER_PRE_RELEASE
        // Enable file logging for pre builds
        Config::Instance()->LogToFile.set_volatile_value(true);

        // Set log level to debug
        if (Config::Instance()->LogLevel.value_or_default() > 1)
            Config::Instance()->LogLevel.set_volatile_value(1);
#endif

        PrepareLogger();
        InstallCrashHandler();

        spdlog::warn("{0} loaded", VER_PRODUCT_NAME);
        spdlog::warn("---------------------------------");
        spdlog::warn("OptiScaler is freely downloadable from");
        spdlog::warn("GitHub : https://github.com/optiscaler/OptiScaler/releases");
        spdlog::warn("Nexus  : https://www.nexusmods.com/site/mods/986");
        spdlog::warn("If you paid for these files, you've been scammed!");
        spdlog::warn("DO NOT USE IN MULTIPLAYER GAMES");
        spdlog::info("");
        spdlog::info("LogLevel: {}", Config::Instance()->LogLevel.value_or_default());

        spdlog::info("");
        if (Util::GetRealWindowsVersion(winVer))
            spdlog::info("Windows version: {} ({}.{}.{})", Util::GetWindowsName(winVer), winVer.dwMajorVersion,
                         winVer.dwMinorVersion, winVer.dwBuildNumber, winVer.dwPlatformId);
        else
            spdlog::warn("Can't read windows version");

        spdlog::info("");

        spdlog::info("Config parameters:");
        for (const std::string& l : Config::Instance()->GetConfigLog())
            spdlog::info(l);

        spdlog::info("");
        spdlog::info("Setting DllPath to {}", wstring_to_string(Config::Instance()->MainDllPath.value()));
        spdlog::info("");

#ifdef VER_PRE_RELEASE
        spdlog::info("Pre-release build, disabling update checks");
        Config::Instance()->CheckForUpdate.set_volatile_value(false);
#endif

        // Initial state of FG
        State::Instance().externalFrameGeneration = Config::Instance()->ExternalFrameGeneration.value_or_default();
        if (State::Instance().externalFrameGeneration)
        {
            // Only runtime overrides: preserve the user's OptiFG configuration for the next
            // startup with External=false. Do not load a second FG provider or change Reflex.
            auto* cfg = Config::Instance();
            cfg->FGInput.set_volatile_value(FGInput::NoFG);
            cfg->FGOutput.set_volatile_value(FGOutput::NoFG);
            cfg->FGNvngxReplacement.set_volatile_value(FGNvngxReplacement::None);
            cfg->FGEnabled.set_volatile_value(false);
            cfg->ForceXeLL.set_volatile_value(false);
            cfg->UseFakenvapi.set_volatile_value(false);
            cfg->FN_ForceReflex.set_volatile_value(ForceReflex::InGame);
            LOG_INFO("External frame generation: leaving Streamline/Reflex and FG control to the game or external module; NR/SR remain available");
        }

        // Init Kernel proxies
        NtdllProxy::Init();
        KernelBaseProxy::Init();
        Kernel32Proxy::Init();

        State::Instance().activeFgInput = Config::Instance()->FGInput.value_or_default();
        if constexpr (!F6InputPolicy::FsrBridgeEnabled)
        {
            auto& input = State::Instance().activeFgInput;
            if (input == FGInput::FSRFG || input == FGInput::FSRFG30)
                input = FGInput::Upscaler; // Effective route only; keep the saved profile intact.
            LOG_INFO("F6 input route: direct DLSS upscaler; FSR bridges/discovery disabled");
        }
        State::Instance().activeFgOutput = Config::Instance()->FGOutput.value_or_default();
        State::Instance().activeFgNvngx = Config::Instance()->FGNvngxReplacement.value_or_default();

        // Ensure valid FG configuration
        if (State::Instance().activeFgInput != FGInput::NvngxFG && State::Instance().activeFgOutput != FGOutput::DLSSG)
            State::Instance().activeFgNvngx = FGNvngxReplacement::None;

        if (State::Instance().activeFgInput == FGInput::NvngxFG)
            State::Instance().activeFgOutput = FGOutput::NoFG;

        // Check for Wine
        spdlog::info("");
        State::Instance().isRunningOnLinux = IsRunningOnWine();

        // Not foolproof
        // calls LoadLibraryExW inside DllMain but seems mostly fine if we only call NvAPI_GetInterfaceVersionString
        auto isNvidiaViaNvapi = [&]()
        {
            bool nvidiaDetected = false;
            const bool nvapiWasLoaded = GetDllNameWModule(&nvapiNamesW) != nullptr;

            // Only try to load the real nvapi which is located in system32
            auto nvapiModule = NtdllProxy::LoadLibraryExW_Ldr(L"nvapi64.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);

            // No nvapi, should not be nvidia
            if (!nvapiModule)
            {
                spdlog::debug("Nvidia detected: {}", nvidiaDetected);
                return nvidiaDetected;
            }

            if (auto o_NvAPI_QueryInterface =
                    (PFN_NvApi_QueryInterface) KernelBaseProxy::GetProcAddress_()(nvapiModule, "nvapi_QueryInterface"))
            {
                // dxvk-nvapi calls CreateDxgiFactory which we can't do because we are inside DLL_PROCESS_ATTACH
                NvAPI_ShortString desc;
                auto* getVersion = GET_INTERFACE(NvAPI_GetInterfaceVersionString, o_NvAPI_QueryInterface);
                if (getVersion && getVersion(desc) == NVAPI_OK &&
                    (std::string_view(desc) == std::string_view("NVAPI Open Source Interface (DXVK-NVAPI)") ||
                     std::string_view(desc) == std::string_view("DXVK_NVAPI")))
                {
                    spdlog::debug("Using dxvk-nvapi");
                    DISPLAY_DEVICEA dd = {};
                    dd.cb = sizeof(dd);
                    int deviceIndex = 0;

                    while (EnumDisplayDevicesA(nullptr, deviceIndex, &dd, 0))
                    {
                        if (dd.StateFlags & DISPLAY_DEVICE_ACTIVE && std::string_view(dd.DeviceID).contains("VEN_10DE"))
                        {
                            // Having any Nvidia GPU active will take precedence
                            nvidiaDetected = true;
                        }
                        deviceIndex++;
                    }
                }
                else if (o_NvAPI_QueryInterface(0x21382138))
                {
                    spdlog::error("Using fakenvapi as nvapi64.dll, remove it!");
                    nvidiaDetected = false;
                }
                else
                {
                    spdlog::debug("Using Nvidia's nvapi");
                    auto init = GET_INTERFACE(NvAPI_Initialize, o_NvAPI_QueryInterface);
                    if (init)
                    {
                        nvidiaDetected = init() == NVAPI_OK;

                        // nvapi64.dll stays loaded once NvAPI_Initialize ran (no NvAPI_Unload, no FreeLibrary): it
                        // leaves driver work scheduled inside nvapi64_impl.dll, and in a game that never loads nvapi
                        // itself (BeamNG.drive on Vulkan) unloading it here lets that work crash the game later.
                        nvapiHeldByStartupCheck = !nvapiWasLoaded;
                        spdlog::debug("Nvidia detected: {}", nvidiaDetected);
                        return nvidiaDetected;
                    }
                }
            }

            NtdllProxy::FreeLibrary_Ldr(nvapiModule);

            spdlog::debug("Nvidia detected: {}", nvidiaDetected);

            return nvidiaDetected;
        };

        bool possibleNvidia = isNvidiaViaNvapi();

        spdlog::info("");
        spdlog::info("Check for DLSS files");

        auto exePath = Util::ExePath().remove_filename();
        auto optiDllPath = std::filesystem::path(Config::Instance()->MainDllPath.value());

        if (Config::Instance()->NVNGX_DLSS_Library.has_value())
        {
            std::filesystem::path dlssPath(Config::Instance()->NVNGX_DLSS_Library.value());
            std::error_code ec;

            // A configured directory names its nvngx_dlss.dll; a file is accepted as given.
            if (std::filesystem::is_directory(dlssPath, ec))
                dlssPath /= L"nvngx_dlss.dll";

            if (std::filesystem::is_regular_file(dlssPath, ec))
            {
                State::Instance().NVNGX_DLSS_Path = dlssPath;
                Config::Instance()->NVNGX_DLSS_Library.set_volatile_value(dlssPath.wstring());
                LOG_INFO("nvngx_dlss.dll from NvngxDlssPath: {}", dlssPath.string());
            }
            else
            {
                LOG_WARN("NvngxDlssPath does not name an existing DLL file: {}", dlssPath.string());
            }
        }

        if (!State::Instance().NVNGX_DLSS_Path.has_value())
            State::Instance().NVNGX_DLSS_Path = Util::FindFilePath(optiDllPath, "nvngx_dlss.dll");

        if (!State::Instance().NVNGX_DLSS_Path.has_value())
            State::Instance().NVNGX_DLSS_Path = Util::FindFilePath(exePath, "nvngx_dlss.dll");

        State::Instance().NVNGX_DLSSD_Path = Util::FindFilePath(optiDllPath, "nvngx_dlssd.dll");
        if (!State::Instance().NVNGX_DLSSD_Path.has_value())
            State::Instance().NVNGX_DLSSD_Path = Util::FindFilePath(exePath, "nvngx_dlssd.dll");

        State::Instance().NVNGX_DLSSG_Path = Util::FindFilePath(optiDllPath, "nvngx_dlssg.dll");
        if (!State::Instance().NVNGX_DLSSG_Path.has_value())
            State::Instance().NVNGX_DLSSG_Path = Util::FindFilePath(exePath, "nvngx_dlssg.dll");

        // Not 100% accurate for Nvidia cards without DLSS
        if (Config::Instance()->DLSSEnabled.value_or_default() && possibleNvidia)
        {
            if (State::Instance().NVNGX_DLSS_Path.has_value())
            {
                spdlog::info("Enabling DLSS");
                Config::Instance()->DLSSEnabled.set_volatile_value(true);
            }
            else
            {
                spdlog::warn("nvngx_dlss.dll not found, disabling DLSS");
                Config::Instance()->DLSSEnabled.set_volatile_value(false);
            }

            // Assumes that dxgi spoofing is only used to enable DLSS
            if (!Config::Instance()->DxgiSpoofing.has_value())
            {
                spdlog::info("Disabling DxgiSpoofing");
                Config::Instance()->DxgiSpoofing.set_volatile_value(false);
            }
        }
        else
        {
            Config::Instance()->DLSSEnabled.set_volatile_value(false);
        }

        spdlog::info("");
        CheckQuirks(possibleNvidia);

        // Check for working mode and attach hooks
        spdlog::info("");
        CheckWorkingMode();
        CheckMemoryForProxies();

        // OptiFG & Overlay Checks
        if ((Config::Instance()->FGInput.value_or_default() == FGInput::Upscaler) &&
            !Config::Instance()->DisableOverlays.has_value())
            Config::Instance()->DisableOverlays.set_volatile_value(true);

        if (Config::Instance()->DisableOverlays.value_or_default())
        {
            _wputenv_s(L"SteamNoOverlayUIDrawing", L"1");
            SetEnvironmentVariableW(L"SteamNoOverlayUIDrawing", L"1");
        }

        // FSR4 Watermark, overrides environment variable only if set in config
        if (Config::Instance()->Fsr4EnableWatermark.has_value())
        {
            if (Config::Instance()->Fsr4EnableWatermark.value())
            {
                _wputenv_s(L"MLSR-WATERMARK", L"1");
                SetEnvironmentVariableW(L"MLSR-WATERMARK", L"1");

                if (!Config::Instance()->FpsOverlayPosition.has_value())
                    Config::Instance()->FpsOverlayPosition.set_volatile_value(FpsOverlayPos_TopRight);
            }
            else
            {
                _wputenv_s(L"MLSR-WATERMARK", L"0");
                SetEnvironmentVariableW(L"MLSR-WATERMARK", L"0");
            }
        }

        if (Config::Instance()->FSRFGEnableWatermark.has_value())
        {
            if (Config::Instance()->FSRFGEnableWatermark.value())
            {
                _wputenv_s(L"MLFI-WATERMARK", L"1");
                SetEnvironmentVariableW(L"MLFI-WATERMARK", L"1");

                if (!Config::Instance()->FpsOverlayPosition.has_value())
                    Config::Instance()->FpsOverlayPosition.set_volatile_value(FpsOverlayPos_TopRight);
            }
            else
            {
                _wputenv_s(L"MLFI-WATERMARK", L"0");
                SetEnvironmentVariableW(L"MLFI-WATERMARK", L"0");
            }
        }

        // Asi plugins
        if (Config::Instance()->LoadAsiPlugins.value_or_default())
        {
            spdlog::info("");
            LoadAsiPlugins();
        }

        if (!Config::Instance()->DxgiSpoofing.has_value() && !State::Instance().nvngxReplacement.has_value())
        {
            LOG_WARN("Nvngx replacement not found!");

            if (!State::Instance().nvngxExists)
            {
                LOG_WARN("nvngx.dll not found! - disabling spoofing");
                Config::Instance()->DxgiSpoofing.set_volatile_value(false);
            }
        }

        if (F6InputPolicy::FsrBridgeEnabled && Config::Instance()->EnableFsr2Inputs.value_or_default())
        {
            spdlog::info("");

            if (Config::Instance()->UseFsr2VulkanInputs.value_or_default())
                HookFSR2VkExeInputs();
            else if (Config::Instance()->UseFsr2Dx11Inputs.value_or_default())
                HookFSR2Dx11ExeInputs();
            else
            {
                handle = GetDllNameWModule(&fsr2NamesW);
                if (handle != nullptr)
                    HookFSR2Inputs(handle);

                handle = GetDllNameWModule(&fsr2BENamesW);
                if (handle != nullptr)
                    HookFSR2Dx12Inputs(handle);

                HookFSR2ExeInputs();
            }
        }

        if (F6InputPolicy::FsrBridgeEnabled && Config::Instance()->EnableFsr3Inputs.value_or_default())
        {
            handle = GetDllNameWModule(&fsr3NamesW);
            if (handle != nullptr)
                HookFSR3Inputs(handle);

            handle = GetDllNameWModule(&fsr3BENamesW);
            if (handle != nullptr)
                HookFSR3Dx12Inputs(handle);

            HookFSR3ExeInputs();
        }
        // HookFfxExeInputs();

        if (F6InputPolicy::FsrBridgeEnabled && State::Instance().activeFgInput == FGInput::FSRFG30)
        {
            FSR3FG::HookFSR3FGInputs();
            FSR3FG::HookFSR3FGExeInputs();
        }

        if (State::Instance().activeFgInput == FGInput::Upscaler &&
            State::Instance().gameEngine == GameEngineType::Unity && !Config::Instance()->FGResourceFlip.has_value())
        {
            LOG_WARN("Unity detected with Upscaler input, but FGResourceFlip is not set. Enabling it");
            Config::Instance()->FGResourceFlip.set_volatile_value(true);
        }

        for (size_t i = 0; i < 300; i++)
        {
            State::Instance().frameTimes.push_back(0.0f);
            State::Instance().upscaleTimes.push_back(0.0f);
        }

        spdlog::info("");
        spdlog::info("Init done");
        spdlog::info("---------------------------------------------");
        spdlog::info("");

        const auto startupThread = CreateThread(nullptr, 0, getGpuInfo, GetDllNameWModule(&dx12NamesW), 0, nullptr);
        if (startupThread)
            CloseHandle(startupThread);
        else
        {
            const auto startupError = GetLastError();
            LOG_ERROR("Startup worker creation failed: {}", startupError);
        }

#ifndef _DEBUG
        if (Config::Instance()->LogLevel.value_or_default() == 0 && Config::Instance()->LogToFile.value_or_default())
        {
            std::thread(
                []()
                {
                    std::this_thread::sleep_for(std::chrono::minutes(10));

                    // If still logging after 10 minutes, send notification
                    if (Config::Instance()->LogLevel.value_or_default() == 0 &&
                        Config::Instance()->LogToFile.value_or_default())
                    {
                        ImGuiToast notification({ ImGuiToastType::Warning, 30000,
                                                  "That's likely unintended and will lead to big OptiScaler.log\n"
                                                  "Please disable logging to file and delete OptiScaler.log" });

                        notification.setTitle("Trace logging still active");

                        ImGui::InsertNotification(notification);
                    }
                })
                .detach();
        }
#endif

        break;
    }

    case DLL_PROCESS_DETACH:
        ModuleLifetime::NotifyDetach();
        State::Instance().isShuttingDown = true;
        // No SDK/COM cleanup, module unloading, logging or waiting under loader
        // lock, including explicit detach. Callback code is pinned above and
        // process termination reclaims the remaining objects and worker handles.
        break;

    case DLL_THREAD_ATTACH:
        // LOG_DEBUG_ONLY("DLL_THREAD_ATTACH from module: {0:X}, count: {1}", (UINT64)hModule, loadCount);
        break;

    case DLL_THREAD_DETACH:
        // LOG_DEBUG_ONLY("DLL_THREAD_DETACH from module: {0:X}, count: {1}", (UINT64)hModule, loadCount);
        break;

    default:
        LOG_WARN("Call reason: {0:X}", ul_reason_for_call);
        break;
    }

    return TRUE;
}
