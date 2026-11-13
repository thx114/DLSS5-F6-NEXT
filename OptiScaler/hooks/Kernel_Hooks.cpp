#include "pch.h"
#include "Kernel_Hooks.h"

#include "Gdi32_Hooks.h"
#include "Streamline_Hooks.h"
#include "LibraryLoad_Hooks.h"

#include <fsr4/FSR4ModelSelection.h>

#include <Util.h>
#include <State.h>
#include <Config.h>
#include <dlssnr/DlssNrFgTrace.h>
#include <runtime/ModuleLifetime.h>

#include <cwctype>
#include <misc/IdentifyGpu.h>

#include "Hook_Utils.h"

#include "Amdxc64_Hooks.h"
#pragma intrinsic(_ReturnAddress)

static bool LoaderHasFsr4Support() noexcept
{
    return IdentifyGpu::getLoaderFsr4Support() != FSR4Support::None;
}


static inline void NormalizePath(std::string& path)
{
    while (!path.empty() && (path.back() == '\\' || path.back() == '/'))
        path.pop_back();
}

static inline bool IsInsideWindowsDirectory(const std::string& path)
{
    char windowsDir[MAX_PATH];
    UINT len = GetWindowsDirectoryA(windowsDir, MAX_PATH);

    if (len == 0 || len >= MAX_PATH)
        return false;

    std::string pathToCheck(path);
    std::string windowsPath(windowsDir);

    NormalizePath(pathToCheck);
    NormalizePath(windowsPath);

    to_lower_in_place(pathToCheck);
    to_lower_in_place(windowsPath);

    // Check if pathToCheck starts with windowsPath, while having a slash after that
    if (pathToCheck.compare(0, windowsPath.size(), windowsPath) == 0 &&
        (pathToCheck.size() == windowsPath.size() || pathToCheck[windowsPath.size()] == '\\' ||
         pathToCheck[windowsPath.size()] == '/'))
        return true;

    return false;
}

VALIDATE_HOOK(hk_K32_GetProcAddress, Kernel32Proxy::PFN_GetProcAddress)
FARPROC WINAPI KernelHooks::hk_K32_GetProcAddress(HMODULE hModule, LPCSTR lpProcName)
{

    if ((size_t) lpProcName < 0x000000000000F000)
    {
        if (hModule == dllModule)
            LOG_TRACE("Ordinal call: {:X}", (size_t) lpProcName);

        return o_K32_GetProcAddress(hModule, lpProcName);
    }

    // if (hModule == dllModule && lpProcName != nullptr)
    //{
    //     LOG_TRACE("Trying to get process address of {}, caller: {}", lpProcName,
    //               Util::WhoIsTheCaller(_ReturnAddress()));
    // }

    // FSR 4 Init in case of missing amdxc64.dll
    // 2nd check is amdxcffx64.dll trying to queue amdxc64 but amdxc64 not being loaded.
    // Also skip the internal call of amdxc64
    if (lpProcName != nullptr && (hModule == amdxc64Mark || hModule == nullptr) &&
        lstrcmpA(lpProcName, "AmdExtD3DCreateInterface") == 0 &&
        LoaderHasFsr4Support() &&
        Util::GetCallerModule(_ReturnAddress()) != KernelBaseProxy::GetModuleHandleW_()(L"amdxc64.dll"))
    {
        LOG_TRACE("Giving hkAmdExtD3DCreateInterface");
        return (FARPROC) &Amdxc64Hooks::hkAmdExtD3DCreateInterface;
    }
    else if (hModule == amdxc64Mark)
    {
        return o_K32_GetProcAddress(KernelBaseProxy::GetModuleHandleW_()(L"amdxc64.dll"), lpProcName);
    }

    const auto resolved = o_K32_GetProcAddress(hModule, lpProcName);
    return !State::Instance().externalFrameGeneration && !State::Instance().isShuttingDown
               ? DlssNrFgTrace::RecordResolved(hModule, lpProcName, resolved, dllModule) : resolved;
}

VALIDATE_HOOK(hk_K32_GetModuleHandleA, Kernel32Proxy::PFN_GetModuleHandleA)
HMODULE WINAPI KernelHooks::hk_K32_GetModuleHandleA(LPCSTR lpModuleName)
{
    if (lpModuleName != NULL)
    {
        if (strcmp(lpModuleName, "nvngx_dlssg.dll") == 0)
        {
            LOG_TRACE("Trying to get module handle of {}, caller: {}", lpModuleName,
                      Util::WhoIsTheCaller(_ReturnAddress()));
            return dllModule;
        }
        else if (strcmp(lpModuleName, "amdxc64.dll") == 0)
        {
            // Libraries like FFX SDK or AntiLag 2 SDK do not load amdxc64 themselves
            // so most likely amdxc64 is getting loaded by the driver itself.
            // Therefore it should be safe for us to return a custom implementation when it's not loaded
            // This can get removed if Proton starts to ship amdxc64

            // For system with amdxc64 loaded - we should've caught the load and hooked it
            // For systems without - we provide that app with a fake handle and track the usage that way

            auto original = o_K32_GetModuleHandleA(lpModuleName);

            const bool fsr4Supported = LoaderHasFsr4Support();

            if (original == nullptr && fsr4Supported)
            {
                LOG_INFO("giving a fake HMODULE for amdxc64.dll");
                return amdxc64Mark;
            }

            return original;
        }
    }

    return o_K32_GetModuleHandleA(lpModuleName);
}

VALIDATE_HOOK(hk_K32_GetModuleHandleW, Kernel32Proxy::PFN_GetModuleHandleW)
HMODULE WINAPI KernelHooks::hk_K32_GetModuleHandleW(LPCWSTR lpModuleName)
{
    if (lpModuleName != NULL)
    {
        if (wcscmp(lpModuleName, L"amdxc64.dll") == 0)
        {
            LOG_TRACE("amdxc64.dll call");

            // See comments in hk_K32_GetModuleHandleA

            auto original = o_K32_GetModuleHandleW(lpModuleName);

            const bool fsr4Supported = LoaderHasFsr4Support();

            if (original == nullptr && fsr4Supported)
            {
                LOG_INFO("giving a fake HMODULE for amdxc64.dll");
                return amdxc64Mark;
            }

            return original;
        }
    }

    return o_K32_GetModuleHandleW(lpModuleName);
}

VALIDATE_HOOK(hk_K32_GetModuleHandleExA, Kernel32Proxy::PFN_GetModuleHandleExA)
BOOL WINAPI KernelHooks::hk_K32_GetModuleHandleExA(DWORD dwFlags, LPCSTR lpModuleName, HMODULE* phModule)
{
    if (lpModuleName && dwFlags == 0 && strcmp("libxell.dll", lpModuleName) == 0 && phModule)
    {
        *phModule = dllModule;
        return true;
    }

    return o_K32_GetModuleHandleExA(dwFlags, lpModuleName, phModule);
}

VALIDATE_HOOK(hk_K32_GetModuleHandleExW, Kernel32Proxy::PFN_GetModuleHandleExW)
BOOL WINAPI KernelHooks::hk_K32_GetModuleHandleExW(DWORD dwFlags, LPCWSTR lpModuleName, HMODULE* phModule)
{
    if (lpModuleName && dwFlags == GET_MODULE_HANDLE_EX_FLAG_PIN && lstrcmpW(L"nvapi64.dll", lpModuleName) == 0 &&
        phModule)
    {
        LOG_TRACE("Suspected SpecialK call for nvapi64");
        *phModule = LibraryLoadHooks::LoadNvApi();
        return true;
    }

    return o_K32_GetModuleHandleExW(dwFlags, lpModuleName, phModule);
}

VALIDATE_HOOK(hk_KB_GetProcAddress, KernelBaseProxy::PFN_GetProcAddress)
FARPROC WINAPI KernelHooks::hk_KB_GetProcAddress(HMODULE hModule, LPCSTR lpProcName)
{
    if ((size_t) lpProcName < 0x000000000000F000)
    {
        if (hModule == dllModule)
            LOG_TRACE("Ordinal call: {:X}", (size_t) lpProcName);

        return o_KB_GetProcAddress(hModule, lpProcName);
    }

    // if (hModule == dllModule && lpProcName != nullptr)
    //{
    //     LOG_TRACE("Trying to get process address of {}, caller: {}", lpProcName,
    //               Util::WhoIsTheCaller(_ReturnAddress()));
    // }

    const auto resolved = o_KB_GetProcAddress(hModule, lpProcName);
    return !State::Instance().externalFrameGeneration && !State::Instance().isShuttingDown
               ? DlssNrFgTrace::RecordResolved(hModule, lpProcName, resolved, dllModule) : resolved;
}

VALIDATE_HOOK(hk_K32_GetFileAttributesW, Kernel32Proxy::PFN_GetFileAttributesW)
DWORD WINAPI KernelHooks::hk_K32_GetFileAttributesW(LPCWSTR lpFileName)
{
    if (!State::Instance().nvngxExists && State::Instance().nvngxReplacement.has_value() &&
        (Config::Instance()->DxgiSpoofing.value_or_default() ||
         Config::Instance()->StreamlineSpoofing.value_or_default()))
    {
        auto path = wstring_to_string(std::wstring(lpFileName));
        to_lower_in_place(path);

        // "nvngx.dll_" excludes OUR neural-rendering forwarder, named nvngx.dll_dlssnr.dll -- it
        // contains the substring "nvngx.dll", so without this the spoof redirects an attempt to open
        // the forwarder to the signed driver nvngx.dll instead, and the wrong image is loaded as the
        // forwarder. That crashed Dragon's Dogma 2 the moment Neural Rendering first initialised.
        if (path.contains("nvngx.dll") && !path.contains("_nvngx.dll") && !path.contains("nvngx.dll_") &&
            !IsInsideWindowsDirectory(path)) // apply the override to just one path
        {
            LOG_DEBUG("Overriding GetFileAttributesW for nvngx");
            return FILE_ATTRIBUTE_ARCHIVE;
        }
    }

    return o_K32_GetFileAttributesW(lpFileName);
}

VALIDATE_HOOK(hk_K32_CreateFileW, Kernel32Proxy::PFN_CreateFileW)
HANDLE WINAPI KernelHooks::hk_K32_CreateFileW(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                                              LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
                                              DWORD dwFlagsAndAttributes, HANDLE hTemplateFile)
{
    if (!State::Instance().nvngxExists && State::Instance().nvngxReplacement.has_value() &&
        (Config::Instance()->DxgiSpoofing.value_or_default() ||
         Config::Instance()->StreamlineSpoofing.value_or_default()))
    {
        auto path = wstring_to_string(std::wstring(lpFileName));
        to_lower_in_place(path);

        // See the note in hk_K32_GetFileAttributesW: exclude our nvngx.dll_dlssnr.dll forwarder.
        if (path.contains("nvngx.dll") && !path.contains("_nvngx.dll") && !path.contains("nvngx.dll_") &&
            !IsInsideWindowsDirectory(path))
        {
            static auto& signedDll = State::Instance().nvngxReplacement;

            if (signedDll.has_value())
            {
                LOG_DEBUG("Overriding CreateFileW for nvngx with a signed dll, original path: {}", path);
                return o_K32_CreateFileW(signedDll.value().c_str(), dwDesiredAccess, dwShareMode, lpSecurityAttributes,
                                         dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
            }
        }
    }

    return o_K32_CreateFileW(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition,
                             dwFlagsAndAttributes, hTemplateFile);
}

VALIDATE_HOOK(hk_K32_SetUnhandledExceptionFilter, Kernel32Proxy::PFN_SetUnhandledExceptionFilter)
LPTOP_LEVEL_EXCEPTION_FILTER WINAPI KernelHooks::hk_K32_SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER lpFilter)
{
    // Logger.cpp keeps its own filter on top and calls the game's after it (the real API is only reached from there)
    return CrashFilterFromGame(lpFilter, o_K32_SetUnhandledExceptionFilter);
}

VALIDATE_HOOK(hk_K32_ExitProcess, Kernel32Proxy::PFN_ExitProcess)
VOID WINAPI KernelHooks::hk_K32_ExitProcess(UINT uExitCode)
{
    // Revoke new work before other DLLs enter their detach callbacks. Do not
    // attempt FG Off, GPU retirement or module destruction on this exit stack.
    ModuleLifetime::RequestProcessExit();
    State::Instance().isShuttingDown = true;
    // Other threads (the log's writer) are stopped by ExitProcess and the loader's detach is skipped for it: this is the
    // last chance for the queued log lines to reach the disk. Bounded.
    DrainLogForExit();
    o_K32_ExitProcess(uExitCode);
}

VALIDATE_HOOK(hk_K32_OutputDebugStringW, Kernel32Proxy::PFN_OutputDebugStringW)
VOID WINAPI KernelHooks::hk_K32_OutputDebugStringW(LPCWSTR lpOutputString)
{
    o_K32_OutputDebugStringW(lpOutputString);

    std::wstring result(lpOutputString);

    while (!result.empty() && (result.back() == L'\n' || result.back() == L'\r'))
    {
        result.pop_back();
    }

    LOG_TRACE(L"{}", result);
}

VALIDATE_HOOK(hk_K32_OutputDebugStringA, Kernel32Proxy::PFN_OutputDebugStringA)
VOID WINAPI KernelHooks::hk_K32_OutputDebugStringA(LPCSTR lpOutputString)
{
    o_K32_OutputDebugStringA(lpOutputString);

    std::string result(lpOutputString);

    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
    {
        result.pop_back();
    }

    LOG_TRACE("{}", result);
}

// Load Library checks

VALIDATE_HOOK(hk_K32_LoadLibraryW, Kernel32Proxy::PFN_LoadLibraryW)
HMODULE KernelHooks::hk_K32_LoadLibraryW(LPCWSTR lpLibFileName)
{
    if (!lpLibFileName) return o_K32_LoadLibraryW(lpLibFileName);
    const auto savedError = GetLastError();
    const std::wstring name = lpLibFileName;
    SetLastError(savedError);
    return LibraryLoadHooks::LoadWin32(name, nullptr, 0, [&](LPCWSTR replacement) {
        if (replacement) return o_K32_LoadLibraryW(replacement);
        return o_K32_LoadLibraryW(lpLibFileName);
    });
}

VALIDATE_HOOK(hk_K32_LoadLibraryA, Kernel32Proxy::PFN_LoadLibraryA)
HMODULE KernelHooks::hk_K32_LoadLibraryA(LPCSTR lpLibFileName)
{
    if (!lpLibFileName) return o_K32_LoadLibraryA(lpLibFileName);
    const auto savedError = GetLastError();
    const std::wstring name = LibraryLoading::AnsiName(lpLibFileName);
    SetLastError(savedError);
    return LibraryLoadHooks::LoadWin32(name, nullptr, 0, [&](LPCWSTR replacement) {
        if (replacement) return Kernel32Proxy::LoadLibraryExW_()(replacement, nullptr, 0);
        return o_K32_LoadLibraryA(lpLibFileName);
    });
}

VALIDATE_HOOK(hk_K32_LoadLibraryExW, Kernel32Proxy::PFN_LoadLibraryExW)
HMODULE KernelHooks::hk_K32_LoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags)
{
    if (!lpLibFileName) return o_K32_LoadLibraryExW(lpLibFileName, hFile, dwFlags);
    const auto savedError = GetLastError();
    const std::wstring name = lpLibFileName;
    SetLastError(savedError);
    return LibraryLoadHooks::LoadWin32(name, hFile, dwFlags, [&](LPCWSTR replacement) {
        if (replacement) return o_K32_LoadLibraryExW(replacement, hFile, dwFlags);
        return o_K32_LoadLibraryExW(lpLibFileName, hFile, dwFlags);
    });
}

VALIDATE_HOOK(hk_K32_LoadLibraryExA, Kernel32Proxy::PFN_LoadLibraryExA)
HMODULE KernelHooks::hk_K32_LoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags)
{
    if (!lpLibFileName) return o_K32_LoadLibraryExA(lpLibFileName, hFile, dwFlags);
    const auto savedError = GetLastError();
    const std::wstring name = LibraryLoading::AnsiName(lpLibFileName);
    SetLastError(savedError);
    return LibraryLoadHooks::LoadWin32(name, hFile, dwFlags, [&](LPCWSTR replacement) {
        if (replacement) return Kernel32Proxy::LoadLibraryExW_()(replacement, hFile, dwFlags);
        return o_K32_LoadLibraryExA(lpLibFileName, hFile, dwFlags);
    });
}

VALIDATE_HOOK(hk_KB_LoadLibraryExW, KernelBaseProxy::PFN_LoadLibraryExW)
HMODULE KernelHooks::hk_KB_LoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags)
{
    if (!lpLibFileName) return o_KB_LoadLibraryExW(lpLibFileName, hFile, dwFlags);
    const auto savedError = GetLastError();
    const std::wstring name = lpLibFileName;
    SetLastError(savedError);
    return LibraryLoadHooks::LoadWin32(name, hFile, dwFlags, [&](LPCWSTR replacement) {
        if (replacement) return o_KB_LoadLibraryExW(replacement, hFile, dwFlags);
        return o_KB_LoadLibraryExW(lpLibFileName, hFile, dwFlags);
    }, LibraryLoading::Layer::KernelBase);
}

VALIDATE_HOOK(hk_K32_FreeLibrary, Kernel32Proxy::PFN_FreeLibrary)
BOOL KernelHooks::hk_K32_FreeLibrary(HMODULE lpLibrary)
{
    if (lpLibrary == nullptr)
        return STATUS_INVALID_PARAMETER;

#ifdef _DEBUG
    // LOG_TRACE("{:X}", (size_t) lpLibrary);
#endif

    if (!State::Instance().isShuttingDown)
    {
        auto result = LibraryLoadHooks::FreeLibrary(lpLibrary);

        if (result.has_value())
            return result.value() == TRUE;
    }

    return o_K32_FreeLibrary(lpLibrary);
}
