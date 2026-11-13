#pragma once

#include "SysUtils.h"

#include <detours/detours.h>
#include <hooks/HookOwnership.h>
#include <runtime/LibraryLoadRequest.h>

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

    // Internal loads use the real Win32 loader, preserving dependency search,
    // resource mapping and errors. The name remains for existing callers.
    static HMODULE LoadLibraryExW_Ldr(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags)
    {
        LibraryLoading::InternalLoad internal(lpLibFileName ? lpLibFileName : L"", hFile, dwFlags);
        return ::LoadLibraryExW(lpLibFileName, hFile, dwFlags);
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

    static PFN_RtlGetVersion& Original_RtlGetVersion() { return HookOwnership::Own::Original<PFN_RtlGetVersion>(_own_RtlGetVersion); }
    static PFN_RtlGetVersion Hook_RtlGetVersion(PVOID method)
    {
        if (_own_RtlGetVersion.live) return Original_RtlGetVersion();
        const auto result = HookOwnership::Own::Install(_own_RtlGetVersion, &(PVOID&) o_RtlGetVersion,
            (PVOID) o_RtlGetVersion, method, _dll, "RtlGetVersion");
        if (result != NO_ERROR) { LOG_ERROR("Failed to hook: {:X}", result); return nullptr; }
        return Original_RtlGetVersion();
    }

    static PFN_LdrLoadDll& Original_LdrLoadDll() { return HookOwnership::Own::Original<PFN_LdrLoadDll>(_own_LdrLoadDll); }
    static PFN_LdrLoadDll Hook_LdrLoadDll(PVOID method)
    {
        if (_own_LdrLoadDll.live) return Original_LdrLoadDll();
        const auto result = HookOwnership::Own::Install(_own_LdrLoadDll, &(PVOID&) o_LdrLoadDll,
            (PVOID) o_LdrLoadDll, method, _dll, "LdrLoadDll");
        if (result != NO_ERROR) { LOG_ERROR("Failed to hook: {:X}", result); return nullptr; }
        return Original_LdrLoadDll();
    }

    static PFN_LdrUnloadDll& Original_LdrUnloadDll() { return HookOwnership::Own::Original<PFN_LdrUnloadDll>(_own_LdrUnloadDll); }
    static PFN_LdrUnloadDll Hook_LdrUnloadDll(PVOID method)
    {
        if (_own_LdrUnloadDll.live) return Original_LdrUnloadDll();
        const auto result = HookOwnership::Own::Install(_own_LdrUnloadDll, &(PVOID&) o_LdrUnloadDll,
            (PVOID) o_LdrUnloadDll, method, _dll, "LdrUnloadDll");
        if (result != NO_ERROR) { LOG_ERROR("Failed to hook: {:X}", result); return nullptr; }
        return Original_LdrUnloadDll();
    }

    static PFN_NtLoadDll& Original_NtLoadDll() { return HookOwnership::Own::Original<PFN_NtLoadDll>(_own_NtLoadDll); }
    static PFN_NtLoadDll Hook_NtLoadDll(PVOID method)
    {
        if (_own_NtLoadDll.live) return Original_NtLoadDll();
        const auto result = HookOwnership::Own::Install(_own_NtLoadDll, &(PVOID&) o_NtLoadDll,
            (PVOID) o_NtLoadDll, method, _dll, "NtLoadDll");
        if (result != NO_ERROR) { LOG_ERROR("Failed to hook: {:X}", result); return nullptr; }
        return Original_NtLoadDll();
    }

  private:
    inline static HookOwnership::Own::Slot _own_RtlGetVersion;
    inline static HookOwnership::Own::Slot _own_LdrLoadDll;
    inline static HookOwnership::Own::Slot _own_LdrUnloadDll;
    inline static HookOwnership::Own::Slot _own_NtLoadDll;
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
