#pragma once

#include "SysUtils.h"

#include <detours/detours.h>
#include <genshin/GenshinOwnHookProvenance.h>

#include <winternl.h>

class NtdllProxy
{
  public:
    typedef NTSTATUS(NTAPI* PFN_LdrLoadDll)(PWSTR PathToFile OPTIONAL, PULONG Flags OPTIONAL,
                                            PUNICODE_STRING ModuleFileName, PHANDLE ModuleHandle);

    typedef NTSTATUS(NTAPI* PFN_NtLoadDll)(PUNICODE_STRING PathToFile OPTIONAL, PULONG Flags OPTIONAL,
                                           PUNICODE_STRING ModuleFileName, PHANDLE ModuleHandle);

    typedef NTSTATUS(NTAPI* PFN_LdrUnloadDll)(PVOID ModuleHandle);

    typedef NTSTATUS(NTAPI* PFN_RtlGetVersion)(PRTL_OSVERSIONINFOW lpVersionInformation);

    // It's a partial implementation
    static HMODULE LoadLibraryExW_Ldr(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags)
    {
        // LdrLoadDll wants a ULONG*, remove unsupported flags
        ULONG ldrFlags = dwFlags & ~(LOAD_WITH_ALTERED_SEARCH_PATH | LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
                                     LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS |
                                     LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);

        // This will receive the module handle:
        HANDLE hModule = nullptr;

        NTSTATUS status;
        if (dwFlags & LOAD_LIBRARY_SEARCH_SYSTEM32)
        {
            static std::filesystem::path sysDir = []
            {
                wchar_t buffer[MAX_PATH];
                GetSystemDirectoryW(buffer, MAX_PATH);
                return std::filesystem::path(buffer);
            }();

            std::filesystem::path sysPath = sysDir / lpLibFileName;

            UNICODE_STRING uName;
            o_RtlInitUnicodeString(&uName, sysPath.c_str());

            status = o_LdrLoadDll(nullptr,                        // null is default search order
                                  ldrFlags ? &ldrFlags : nullptr, // optional flags
                                  &uName,                         // the name of the DLL
                                  &hModule                        // out: module handle
            );
        }
        else
        {
            UNICODE_STRING uName;
            o_RtlInitUnicodeString(&uName, lpLibFileName);

            status = o_LdrLoadDll(nullptr,                        // null is default search order
                                  ldrFlags ? &ldrFlags : nullptr, // optional flags
                                  &uName,                         // the name of the DLL
                                  &hModule                        // out: module handle
            );
        }

        if (NT_SUCCESS(status))
        {
            return static_cast<HMODULE>(hModule);
        }
        else
        {
            // translate NTSTATUS to a Win32 error code:
            SetLastError(o_RtlNtStatusToDosError(status));
            return nullptr;
        }
    }

    static NTSTATUS FreeLibrary_Ldr(PVOID handle) { return o_LdrUnloadDll(handle); }

    static void Init()
    {
        if (o_RtlInitUnicodeString != nullptr)
            return;

        _dll = GetModuleHandleW(L"ntdll.dll");

        if (_dll == nullptr)
            return;

        o_RtlInitUnicodeString = (PFN_RtlInitUnicodeString) GetProcAddress(_dll, "RtlInitUnicodeString");
        o_RtlNtStatusToDosError = (PFN_RtlNtStatusToDosError) GetProcAddress(_dll, "RtlNtStatusToDosError");
        o_RtlGetVersion = (PFN_RtlGetVersion) GetProcAddress(_dll, "RtlGetVersion");
        o_LdrLoadDll = (PFN_LdrLoadDll) GetProcAddress(_dll, "LdrLoadDll");
        o_LdrUnloadDll = (PFN_LdrUnloadDll) GetProcAddress(_dll, "LdrUnloadDll");
        o_NtLoadDll = (PFN_NtLoadDll) GetProcAddress(_dll, "NtLoadDll");
    }

    static HMODULE Module() { return _dll; }

    static PFN_RtlGetVersion& Original_RtlGetVersion() { return GenshinNativeQuery::Own::Original<PFN_RtlGetVersion>(_own_RtlGetVersion); }
    static PFN_RtlGetVersion Hook_RtlGetVersion(PVOID method)
    {
        if (_own_RtlGetVersion.live) return Original_RtlGetVersion();
        const auto result = GenshinNativeQuery::Own::Install(_own_RtlGetVersion, &(PVOID&) o_RtlGetVersion,
            (PVOID) o_RtlGetVersion, method, _dll, "RtlGetVersion");
        if (result != NO_ERROR) { LOG_ERROR("Failed to hook: {:X}", result); return nullptr; }
        return Original_RtlGetVersion();
    }

    static PFN_LdrLoadDll& Original_LdrLoadDll() { return GenshinNativeQuery::Own::Original<PFN_LdrLoadDll>(_own_LdrLoadDll); }
    static PFN_LdrLoadDll Hook_LdrLoadDll(PVOID method)
    {
        if (_own_LdrLoadDll.live) return Original_LdrLoadDll();
        const auto result = GenshinNativeQuery::Own::Install(_own_LdrLoadDll, &(PVOID&) o_LdrLoadDll,
            (PVOID) o_LdrLoadDll, method, _dll, "LdrLoadDll");
        if (result != NO_ERROR) { LOG_ERROR("Failed to hook: {:X}", result); return nullptr; }
        return Original_LdrLoadDll();
    }

    static PFN_LdrUnloadDll& Original_LdrUnloadDll() { return GenshinNativeQuery::Own::Original<PFN_LdrUnloadDll>(_own_LdrUnloadDll); }
    static PFN_LdrUnloadDll Hook_LdrUnloadDll(PVOID method)
    {
        if (_own_LdrUnloadDll.live) return Original_LdrUnloadDll();
        const auto result = GenshinNativeQuery::Own::Install(_own_LdrUnloadDll, &(PVOID&) o_LdrUnloadDll,
            (PVOID) o_LdrUnloadDll, method, _dll, "LdrUnloadDll");
        if (result != NO_ERROR) { LOG_ERROR("Failed to hook: {:X}", result); return nullptr; }
        return Original_LdrUnloadDll();
    }

    static PFN_NtLoadDll& Original_NtLoadDll() { return GenshinNativeQuery::Own::Original<PFN_NtLoadDll>(_own_NtLoadDll); }
    static PFN_NtLoadDll Hook_NtLoadDll(PVOID method)
    {
        if (_own_NtLoadDll.live) return Original_NtLoadDll();
        const auto result = GenshinNativeQuery::Own::Install(_own_NtLoadDll, &(PVOID&) o_NtLoadDll,
            (PVOID) o_NtLoadDll, method, _dll, "NtLoadDll");
        if (result != NO_ERROR) { LOG_ERROR("Failed to hook: {:X}", result); return nullptr; }
        return Original_NtLoadDll();
    }

  private:
    inline static GenshinNativeQuery::Own::Slot _own_RtlGetVersion;
    inline static GenshinNativeQuery::Own::Slot _own_LdrLoadDll;
    inline static GenshinNativeQuery::Own::Slot _own_LdrUnloadDll;
    inline static GenshinNativeQuery::Own::Slot _own_NtLoadDll;
    typedef decltype(&RtlInitUnicodeString) PFN_RtlInitUnicodeString;
    typedef decltype(&RtlNtStatusToDosError) PFN_RtlNtStatusToDosError;

    inline static HMODULE _dll = nullptr;

    inline static PFN_LdrLoadDll o_LdrLoadDll = nullptr;
    inline static PFN_LdrUnloadDll o_LdrUnloadDll = nullptr;
    inline static PFN_NtLoadDll o_NtLoadDll = nullptr;
    inline static PFN_RtlInitUnicodeString o_RtlInitUnicodeString = nullptr;
    inline static PFN_RtlNtStatusToDosError o_RtlNtStatusToDosError = nullptr;
    inline static PFN_RtlGetVersion o_RtlGetVersion = nullptr;
};
